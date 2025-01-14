/*
 * Copyright (C) 2006 Zack Rusin <zack@kde.org>
 * Copyright (C) 2006, 2007 Apple Inc.  All rights reserved.
 * Copyright (C) 2008 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) 2013 Cisco Systems, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "Pasteboard.h"

#include "CachedImage.h"
#include "DocumentFragment.h"
#include "DragData.h"
#include "Editor.h"
#include "Frame.h"
#include "Image.h"
#include "NotImplemented.h"
#include "RenderImage.h"
#include "markup.h"
#include <qclipboard.h>
#include <qdebug.h>
#include <qguiapplication.h>
#include <qmimedata.h>
#include <qtextcodec.h>
#include <qurl.h>

#define methodDebug() qDebug() << "PasteboardQt: " << __FUNCTION__;

namespace WebCore {

static bool isTextMimeType(const String& type)
{
    return type == "text/plain" || type.startsWith("text/plain;");
}

static bool isHtmlMimeType(const String& type)
{
    return type == "text/html" || type.startsWith("text/html;");
}

std::unique_ptr<Pasteboard> Pasteboard::create(const QMimeData* readableClipboard, bool isForDragAndDrop)
{
    return std::make_unique<Pasteboard>(readableClipboard, isForDragAndDrop);
}

std::unique_ptr<Pasteboard> Pasteboard::createForCopyAndPaste()
{
#ifndef QT_NO_CLIPBOARD
    return create(QGuiApplication::clipboard()->mimeData());
#else
    return create();
#endif
}

std::unique_ptr<Pasteboard> Pasteboard::createForGlobalSelection()
{
    auto pasteboard = createForCopyAndPaste();
    pasteboard->m_selectionMode = true;
    return pasteboard;
}

std::unique_ptr<Pasteboard> Pasteboard::createPrivate()
{
    return create();
}

#if ENABLE(DRAG_SUPPORT)
std::unique_ptr<Pasteboard> Pasteboard::createForDragAndDrop()
{
    return create(0, true);
}

std::unique_ptr<Pasteboard> Pasteboard::createForDragAndDrop(const DragData& dragData)
{
    return create(dragData.platformData(), true);
}
#endif

Pasteboard::Pasteboard(const QMimeData* readableClipboard, bool isForDragAndDrop)
    : m_selectionMode(false)
    , m_readableData(readableClipboard)
    , m_writableData(0)
    , m_isForDragAndDrop(isForDragAndDrop)
{
}

Pasteboard::~Pasteboard()
{
    if (m_writableData && isForCopyAndPaste())
        m_writableData = 0;
    else
        delete m_writableData;
    m_readableData = 0;
}

void Pasteboard::writeSelection(Range& selectedRange, bool canSmartCopyOrDelete, Frame& frame, ShouldSerializeSelectedTextForDataTransfer shouldSerializeSelectedTextForDataTransfer)
{
    QMimeData* md = new QMimeData;
    QString text = shouldSerializeSelectedTextForDataTransfer == IncludeImageAltTextForDataTransfer ? frame.editor().selectedTextForDataTransfer() : frame.editor().selectedText();
    text.replace(QChar(0xa0), QLatin1Char(' '));
    md->setText(text);

    QString markup = createMarkup(selectedRange, 0, AnnotateForInterchange, false, ResolveNonLocalURLs);
#ifdef Q_OS_MAC
    markup.prepend(QLatin1String("<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" /></head><body>"));
    markup.append(QLatin1String("</body></html>"));
    md->setData(QLatin1String("text/html"), markup.toUtf8());
#else
    md->setHtml(markup);
#endif

    if (canSmartCopyOrDelete)
        md->setData(QLatin1String("application/vnd.qtwebkit.smartpaste"), QByteArray());
#ifndef QT_NO_CLIPBOARD
    QGuiApplication::clipboard()->setMimeData(md, m_selectionMode ? QClipboard::Selection : QClipboard::Clipboard);
#endif
}

bool Pasteboard::canSmartReplace()
{
#ifndef QT_NO_CLIPBOARD
    if (QGuiApplication::clipboard()->mimeData()->hasFormat((QLatin1String("application/vnd.qtwebkit.smartpaste"))))
        return true;
#endif
    return false;
}

void Pasteboard::read(PasteboardPlainText& text)
{
#ifndef QT_NO_CLIPBOARD
    text.text = QGuiApplication::clipboard()->text(m_selectionMode ? QClipboard::Selection : QClipboard::Clipboard);
#endif
}

PassRefPtr<DocumentFragment> Pasteboard::documentFragment(Frame& frame, Range& context,
                                                          bool allowPlainText, bool& chosePlainText)
{
#ifndef QT_NO_CLIPBOARD
    const QMimeData* mimeData = QGuiApplication::clipboard()->mimeData(
            m_selectionMode ? QClipboard::Selection : QClipboard::Clipboard);

    chosePlainText = false;

    if (mimeData->hasHtml()) {
        QString html = mimeData->html();
        if (!html.isEmpty()) {
            RefPtr<DocumentFragment> fragment = createFragmentFromMarkup(*frame.document(), html, "", DisallowScriptingAndPluginContent);
            if (fragment)
                return fragment.release();
        }
    }

    if (allowPlainText && mimeData->hasText()) {
        chosePlainText = true;
        RefPtr<DocumentFragment> fragment = createFragmentFromText(context, mimeData->text());
        if (fragment)
            return fragment.release();
    }
#endif
    return nullptr;
}

void Pasteboard::writePlainText(const String& text, SmartReplaceOption smartReplaceOption)
{
#ifndef QT_NO_CLIPBOARD
    QMimeData* md = new QMimeData;
    QString qtext = text;
    qtext.replace(QChar(0xa0), QLatin1Char(' '));
    md->setText(qtext);
    if (smartReplaceOption == CanSmartReplace)
        md->setData(QLatin1String("application/vnd.qtwebkit.smartpaste"), QByteArray());
    QGuiApplication::clipboard()->setMimeData(md, m_selectionMode ? QClipboard::Selection : QClipboard::Clipboard);
#endif
}

void Pasteboard::write(const PasteboardURL& pasteboardURL)
{
    ASSERT(!pasteboardURL.url.isEmpty());

#ifndef QT_NO_CLIPBOARD
    QMimeData* md = new QMimeData;
    QString urlString = pasteboardURL.url.string();
    md->setText(urlString);
    md->setUrls(QList<QUrl>() << pasteboardURL.url);
    QGuiApplication::clipboard()->setMimeData(md, m_selectionMode ? QClipboard::Selection : QClipboard::Clipboard);
#endif
}

void Pasteboard::writeImage(Element& node, const URL&, const String&)
{
    if (!(node.renderer() && node.renderer()->isImage()))
        return;

#ifndef QT_NO_CLIPBOARD
    CachedImage* cachedImage = downcast<RenderImage>(node.renderer())->cachedImage();
    if (!cachedImage || cachedImage->errorOccurred())
        return;

    Image* image = cachedImage->imageForRenderer(node.renderer());
    ASSERT(image);

    QPixmap* pixmap = image->nativeImageForCurrentFrame();
    if (!pixmap)
        return;
    QGuiApplication::clipboard()->setPixmap(*pixmap, QClipboard::Clipboard);
#endif
}

const QMimeData* Pasteboard::readData() const
{
    ASSERT(!(m_readableData && m_writableData));
    return m_readableData ? m_readableData : m_writableData;
}

bool Pasteboard::hasData()
{
    const QMimeData *data = readData();
    if (!data)
        return false;
    return data->formats().count() > 0;
}

void Pasteboard::clear(const String& type)
{
    if (m_writableData) {
        m_writableData->removeFormat(type);
        if (m_writableData->formats().isEmpty()) {
            if (isForDragAndDrop())
                delete m_writableData;
            m_writableData = 0;
        }
    }
#ifndef QT_NO_CLIPBOARD
    if (isForCopyAndPaste())
        QGuiApplication::clipboard()->setMimeData(m_writableData);
#endif
}

void Pasteboard::clear()
{
#ifndef QT_NO_CLIPBOARD
    if (isForCopyAndPaste())
        QGuiApplication::clipboard()->setMimeData(0);
    else
#endif
    delete m_writableData;
    m_writableData = 0;
}

String Pasteboard::readString(const String& type)
{
    const QMimeData* data = readData();
    if (!data)
        return String();

    if (isHtmlMimeType(type) && data->hasHtml())
        return data->html();

    if (isTextMimeType(type) && data->hasText())
        return data->text();

    QByteArray rawData = data->data(type);
    QString stringData = QTextCodec::codecForName("UTF-16")->toUnicode(rawData);
    return stringData;
}

void Pasteboard::writeString(const String& type, const String& data)
{
    if (!m_writableData)
        m_writableData = new QMimeData;

    if (isTextMimeType(type))
        m_writableData->setText(QString(data));
    else if (isHtmlMimeType(type))
        m_writableData->setHtml(QString(data));
    else {
        // FIXME: we may want to transfer String in UTF8
        QByteArray array(reinterpret_cast<const char*>(data.characters16()), data.length() * 2);
        m_writableData->setData(QString(type), array);
    }
}

Vector<String> Pasteboard::types()
{
    const QMimeData* data = readData();
    if (!data)
        return Vector<String>();

    ListHashSet<String> result;
    QStringList formats = data->formats();
    for (int i = 0; i < formats.count(); ++i)
        result.add(formats.at(i));
    Vector<String> vector;
    copyToVector(result, vector);
    return vector;
}

Vector<String> Pasteboard::readFilenames()
{
    const QMimeData* data = readData();
    if (!data)
        return Vector<String>();

    Vector<String> fileList;
    QList<QUrl> urls = data->urls();

    for (int i = 0; i < urls.size(); i++) {
        QUrl url = urls[i];
        if (url.scheme() != QLatin1String("file"))
            continue;
        fileList.append(url.toLocalFile());
    }

    return fileList;
}

#if ENABLE(DRAG_SUPPORT)
void Pasteboard::setDragImage(DragImageRef, const IntPoint& hotSpot)
{
    notImplemented();
}
#endif

void Pasteboard::writePasteboard(const Pasteboard& sourcePasteboard)
{
#ifndef QT_NO_CLIPBOARD
    QGuiApplication::clipboard()->setMimeData(sourcePasteboard.clipboardData());
#endif
}

}
