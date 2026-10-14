// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/FlagsEnum.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/Link.hpp"
#include "messages/MessageColor.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QString>

#include <climits>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QPainter;

namespace chatterino {
class MessageElement;
class Image;
using ImagePtr = std::shared_ptr<Image>;
enum class FontStyle : uint8_t;
enum class MessageElementFlag : int64_t;
struct MessageColors;
struct AnimatedMessageShadow;

/// A match within one layout element
struct FragmentHighlight {
    /// For text, `start` and `length` are relative to the element's text
    HighlightMatch match;

    /// The match goes on in the element before or after this one
    bool connectsBefore = false;
    bool connectsAfter = false;
};

/// The part of a marker that goes behind the text
void paintFragmentHighlightBackground(QPainter &painter, const QRectF &rect,
                                      const FragmentHighlight &highlight);
/// The part of a marker that goes over the text
void paintFragmentHighlightForeground(QPainter &painter, const QRectF &rect,
                                      const FragmentHighlight &highlight);

class MessageLayoutElement
{
public:
    MessageLayoutElement(MessageElement &creator_, QSizeF size);
    virtual ~MessageLayoutElement();

    MessageLayoutElement(const MessageLayoutElement &) = delete;
    MessageLayoutElement &operator=(const MessageLayoutElement &) = delete;

    MessageLayoutElement(MessageLayoutElement &&) = delete;
    MessageLayoutElement &operator=(MessageLayoutElement &&) = delete;

    bool reversedNeutral = false;

    const QRectF &getRect() const;
    MessageElement &getCreator() const;
    void setPosition(QPointF point);
    bool hasTrailingSpace() const;
    size_t getLine() const;
    void setLine(size_t line);

    MessageLayoutElement *setTrailingSpace(bool value);

    MessageLayoutElement *setLink(const Link &link);

    MessageLayoutElement *setText(const QString &text_);

    virtual void addCopyTextToString(QString &str, uint32_t from = 0,
                                     uint32_t to = UINT32_MAX) const = 0;
    virtual size_t getSelectionIndexCount() const = 0;
    virtual void paint(QPainter &painter,
                       const MessageColors &messageColors) = 0;

    /// Paints what animates and returns the area that was painted.
    /// @param shadow the theme's shadow to draw behind it, if any
    virtual QRegion paintAnimated(
        QPainter &painter, qreal yOffset,
        const AnimatedMessageShadow *shadow = nullptr) = 0;
    virtual int getMouseOverIndex(QPointF abs) const = 0;
    virtual qreal getXFromIndex(size_t index) = 0;

    Link getLink() const;
    const QString &getText() const;
    FlagsEnum<MessageElementFlag> getFlags() const;

    int getWordId() const;
    void setWordId(int wordId);

    /// What to say about the marker at `point`, if there is one there.
    /// `point` is relative to the message, like the element's rect.
    virtual QString getFragmentTooltip(QPointF point) const;

protected:
    bool trailingSpace = true;

private:
    QString text_;
    QRectF rect_;
    std::optional<Link> link_;
    MessageElement &creator_;

    size_t line_{};

    int wordId_ = -1;
};

class ImageLayoutElement : public MessageLayoutElement
{
public:
    ImageLayoutElement(MessageElement &creator, ImagePtr image, QSizeF size);

    void setFragmentHighlight(FragmentHighlight highlight);
    QString getFragmentTooltip(QPointF point) const override;

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

    ImagePtr image_;
    std::unique_ptr<FragmentHighlight> fragmentHighlight_;
};

class LayeredImageLayoutElement : public MessageLayoutElement
{
public:
    LayeredImageLayoutElement(MessageElement &creator,
                              std::vector<ImagePtr> images,
                              std::vector<QSizeF> sizes, QSizeF largestSize,
                              uint32_t modifierFlags = 0);

    bool removesPreviousSpace() const;

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

private:
    bool needsAnimatedPaint() const;
    QRegion paintModified(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow = nullptr);

    std::vector<ImagePtr> images_;
    std::vector<QSizeF> sizes_;
    QSizeF contentSize_;
    uint32_t modifierFlags_ = 0;
};

class ImageWithBackgroundLayoutElement : public ImageLayoutElement
{
public:
    ImageWithBackgroundLayoutElement(MessageElement &creator, ImagePtr image,
                                     QSizeF size, QColor color);

protected:
    void paint(QPainter &painter, const MessageColors &messageColors) override;

private:
    QColor color_;
};

class ImageWithCircleBackgroundLayoutElement : public ImageLayoutElement
{
public:
    ImageWithCircleBackgroundLayoutElement(MessageElement &creator,
                                           ImagePtr image,
                                           const QSize &imageSize, QColor color,
                                           int padding);

protected:
    void paint(QPainter &painter, const MessageColors &messageColors) override;

private:
    const QColor color_;
    const QSize imageSize_;
    const int padding_;
};

class TextLayoutElement : public MessageLayoutElement
{
public:
    TextLayoutElement(MessageElement &creator_, QString &text, QSizeF size,
                      QColor color_, FontStyle style_,
                      MessageColor::Type messageColor, float scale_,
                      float dpr = 1.0F);

    void setFragmentHighlights(std::vector<FragmentHighlight> highlights);
    QString getFragmentTooltip(QPointF point) const override;

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

    QColor color_;
    FontStyle style_;
    // 7tv: this is used to check for system messages - it doesn't take extra
    // space (fits in the padding of `style_`)
    MessageColor::Type messageColor_;
    float scale_;
    float dpr_ = 1.0F;  // for 7tv paints

private:
    void paintFragmentHighlights(QPainter &painter, bool foreground);
    /// Where the characters from `from` up to `to` are
    QRectF fragmentRect(qsizetype from, qsizetype to) const;

    std::vector<FragmentHighlight> fragmentHighlights_;
};

class TextIconLayoutElement : public MessageLayoutElement
{
public:
    TextIconLayoutElement(MessageElement &creator_, const QString &line1,
                          const QString &line2, float scale, QSizeF size);

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

private:
    float scale;
    QString line1;
    QString line2;
};

class VoiceMessageLayoutElement : public MessageLayoutElement
{
public:
    VoiceMessageLayoutElement(MessageElement &creator, QString voiceId,
                              QSizeF size, float scale);
    bool isOverPlayButton(QPointF point) const;
    std::optional<double> seekProgressAt(QPointF point) const;

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

private:
    qreal barHeight(int index, int count) const;
    QRectF playButtonRect() const;
    QRectF waveformRect() const;

    QString voiceId_;
    float scale_;
};

class ReplyCurveLayoutElement : public MessageLayoutElement
{
public:
    ReplyCurveLayoutElement(MessageElement &creator, qreal width,
                            float thickness, float radius, float neededMargin);

protected:
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    QRegion paintAnimated(QPainter &painter, qreal yOffset,
                          const AnimatedMessageShadow *shadow) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;

private:
    const QPen pen_;
    const float radius_;
    const float neededMargin_;
};

}  // namespace chatterino
