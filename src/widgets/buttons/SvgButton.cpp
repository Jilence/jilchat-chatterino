// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/buttons/SvgButton.hpp"

#include "singletons/Theme.hpp"

#include <QHash>
#include <QSvgRenderer>

#include <memory>

namespace {

/// One parsed renderer per SVG file for the whole app. Every split has the
/// same buttons; parsing the file per button (and for splits in tabs that
/// are never looked at) made up a large part of restoring the window.
QSvgRenderer *sharedRenderer(const QString &path)
{
    static QHash<QString, std::shared_ptr<QSvgRenderer>> renderers;
    auto &renderer = renderers[path];
    if (!renderer)
    {
        renderer = std::make_shared<QSvgRenderer>(path);
        renderer->setAspectRatioMode(Qt::KeepAspectRatio);
    }
    return renderer.get();
}

}  // namespace

namespace chatterino {

SvgButton::SvgButton(Src source, BaseWidget *parent, QSize padding)
    : Button(parent)
    , source_(std::move(source))
    , padding_(padding)
{
    this->setContentCacheEnabled(true);
}

void SvgButton::setSource(Src source)
{
    this->source_ = std::move(source);
    this->loadSource();
    this->invalidateContent();
}

void SvgButton::setColor(std::optional<QColor> color)
{
    this->color_ = color;
    this->invalidateContent();
}

void SvgButton::setPadding(QSize padding)
{
    if (this->padding_ == padding)
    {
        return;
    }

    this->padding_ = padding;
    this->invalidateContent();
}

void SvgButton::themeChangedEvent()
{
    Button::themeChangedEvent();

    if (this->source_.dark == this->source_.light)
    {
        return;
    }
    this->loadSource();
    this->invalidateContent();
}

void SvgButton::scaleChangedEvent(float scale)
{
    Button::scaleChangedEvent(scale);

    this->invalidateContent();
}

void SvgButton::resizeEvent(QResizeEvent *e)
{
    Button::resizeEvent(e);

    this->invalidateContent();
}

void SvgButton::paintContent(QPainter &painter)
{
    QSize actualPadding = this->scale() * this->padding_;
    QPoint topLeft{actualPadding.width(), actualPadding.height()};
    QSize contentSize = this->size() - 2 * actualPadding;
    auto bounds = QRectF{topLeft, contentSize};
    // Looked up when first drawn: buttons of hidden tabs never parse it.
    if (this->svg_ == nullptr)
    {
        this->svg_ = sharedRenderer(this->currentSvgPath());
    }
    this->svg_->render(&painter, bounds);

    if (this->color_.has_value())
    {
        painter.save();

        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(bounds, *this->color_);

        painter.restore();
    }
}

QString SvgButton::currentSvgPath() const
{
    if (this->theme->isLightTheme())
    {
        return this->source_.light;
    }
    return this->source_.dark;
}

void SvgButton::loadSource()
{
    // The shared renderer for the current path is looked up on the next
    // paint.
    this->svg_ = nullptr;
}

}  // namespace chatterino
