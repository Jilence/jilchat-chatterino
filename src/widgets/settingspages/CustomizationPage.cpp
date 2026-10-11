#include "widgets/settingspages/CustomizationPage.hpp"

#include "Application.hpp"
#include "common/Version.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTabWidget>
#include <QTextLayout>
#include <QTimer>
#include <QtMath>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <optional>
#include <vector>

using namespace Qt::StringLiterals;

namespace chatterino {

namespace {

// Theme files use the format of Moltorino, so themes can be exchanged.
constexpr auto APPEARANCE_FORMAT = "moltorino-appearance";
constexpr int APPEARANCE_VERSION = 1;
constexpr qint64 MAX_THEME_FILE_SIZE = 2LL * 1024 * 1024;
constexpr qint64 MAX_WALLPAPER_FILE_SIZE = 50LL * 1024 * 1024;

std::optional<QString> promptForThemeName(QWidget *parent, const QString &title,
                                          const QString &label,
                                          const QString &currentName)
{
    QInputDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(currentName);
    dialog.setTextEchoMode(QLineEdit::Normal);
    dialog.setMinimumWidth(360);
    if (auto *input = dialog.findChild<QLineEdit *>())
    {
        input->setMinimumWidth(320);
        input->selectAll();
    }
    dialog.resize(360, dialog.sizeHint().height());

    if (dialog.exec() != QDialog::Accepted)
    {
        return std::nullopt;
    }
    return dialog.textValue();
}

QString safeFileBase(QString name)
{
    static const QRegularExpression unsafe(u"[^a-z0-9]+"_s);
    static const QRegularExpression edges(u"(^-+|-+$)"_s);
    name = name.trimmed().toLower();
    name.replace(unsafe, u"-"_s);
    name.remove(edges);
    return name.isEmpty() ? u"custom-theme"_s : name.left(48);
}

QString importedThemeName(const QString &path)
{
    static const QRegularExpression separators(u"[_-]+"_s);
    auto name = QFileInfo(path).completeBaseName();
    if (name.endsWith(u".moltheme"_s, Qt::CaseInsensitive))
    {
        name.chop(9);
    }
    name.replace(separators, u" "_s);
    name = name.simplified().left(80);
    if (name.isEmpty())
    {
        return u"Imported theme"_s;
    }
    name[0] = name[0].toUpper();
    return name;
}

/// The path as it compares to others: absolute, links resolved, and without
/// regard to case where the file system has none.
QString normalizedLocalPath(const QString &path)
{
    const QFileInfo info(path);
    auto resolved = info.canonicalFilePath();
    if (resolved.isEmpty())
    {
        resolved = info.absoluteFilePath();
    }
    resolved = QDir::fromNativeSeparators(QDir::cleanPath(resolved));
#ifdef Q_OS_WIN
    resolved = resolved.toCaseFolded();
#endif
    return resolved;
}

QString wallpaperFileFilter()
{
    QStringList patterns;
    for (const auto &format : QImageReader::supportedImageFormats())
    {
        const auto name = QString::fromLatin1(format).toLower();
        if (name == u"png" || name == u"jpg" || name == u"jpeg" ||
            name == u"webp" || name == u"gif" || name == u"bmp")
        {
            patterns.append(u"*."_s + name);
        }
    }
    patterns.removeDuplicates();
    patterns.sort();
    return u"Images (%1)"_s.arg(patterns.join(u' '));
}

/// The fonts of the previews. The real ones are built by Fonts, which also
/// honors the "bold scale" setting for usernames.
QFont makeResolvedFont(const QString &family, qreal pointSize, int weight,
                       bool italic = false)
{
    QFont font(family);
    font.setPointSizeF(std::max<qreal>(1, pointSize));
    font.setWeight(static_cast<QFont::Weight>(
        std::clamp(weight, static_cast<int>(QFont::Thin),
                   static_cast<int>(QFont::Black))));
    font.setItalic(italic);
    return font;
}

QFont makeResolvedFont(const QFont &base, int weight)
{
    auto font = base;
    font.setWeight(static_cast<QFont::Weight>(
        std::clamp(weight, static_cast<int>(QFont::Thin),
                   static_cast<int>(QFont::Black))));
    return font;
}

int getUsernameBoldness()
{
    return QFont::Bold;
}

/// A slider that jumps to where it is clicked.
class PositionSlider final : public QSlider
{
public:
    explicit PositionSlider(QWidget *parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        this->setTracking(true);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            QSlider::mousePressEvent(event);
            return;
        }
        this->setSliderDown(true);
        this->setFromPosition(event->position().x());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!this->isSliderDown())
        {
            QSlider::mouseMoveEvent(event);
            return;
        }
        this->setFromPosition(event->position().x());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && this->isSliderDown())
        {
            this->setFromPosition(event->position().x());
            this->setSliderDown(false);
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            this->setFromPosition(event->position().x());
            event->accept();
            return;
        }
        QSlider::mouseDoubleClickEvent(event);
    }

private:
    void setFromPosition(qreal x)
    {
        QStyleOptionSlider option;
        this->initStyleOption(&option);
        const auto groove = this->style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const auto handle = this->style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        const int span = std::max(1, groove.width() - handle.width());
        const int position =
            std::clamp(qRound(x) - groove.left() - handle.width() / 2, 0, span);
        this->setValue(
            QStyle::sliderValueFromPosition(this->minimum(), this->maximum(),
                                            position, span, option.upsideDown));
    }
};

QWidget *makeOpacityAdjustmentSlider(QSlider **slider)
{
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *less = new QLabel(u"Fainter"_s, container);
    auto *more = new QLabel(u"Stronger"_s, container);
    auto *value = new QLabel(u"Original"_s, container);
    value->setMinimumWidth(58);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    *slider = new PositionSlider(container);
    (*slider)->setRange(-100, 100);
    (*slider)->setSingleStep(1);
    (*slider)->setPageStep(10);
    (*slider)->setValue(0);
    QObject::connect(
        *slider, &QSlider::valueChanged, value, [value](int amount) {
            value->setText(amount == 0 ? u"Original"_s
                                       : QStringLiteral("%1%2%").arg(
                                             amount > 0 ? u"+"_s : QString(),
                                             QString::number(amount)));
        });

    layout->addWidget(less);
    layout->addWidget(*slider, 1);
    layout->addWidget(more);
    layout->addWidget(value);
    return container;
}

QWidget *makeSlider(int minimum, int maximum, QSlider **slider,
                    const QString &suffix = {})
{
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    *slider = new PositionSlider(container);
    (*slider)->setRange(minimum, maximum);
    auto *value = new QLabel(container);
    value->setMinimumWidth(42);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value->setText(QString::number(minimum) + suffix);
    QObject::connect(*slider, &QSlider::valueChanged, value,
                     [value, suffix](int amount) {
                         value->setText(QString::number(amount) + suffix);
                     });
    layout->addWidget(*slider, 1);
    layout->addWidget(value);
    return container;
}

QColor nestedColor(const QJsonObject &root, std::initializer_list<QString> path,
                   const QColor &fallback)
{
    QJsonValue value(root);
    for (const auto &part : path)
    {
        value = value.toObject().value(part);
    }
    const QColor color(value.toString());
    return color.isValid() ? color : fallback;
}

ThemeCustomizationProfile profileFromTheme(const ThemeDescriptor &descriptor,
                                           const QJsonObject &json)
{
    if (auto profile = customizationProfileFromTheme(json))
    {
        return *profile;
    }
    return customizationProfileFromClassicTheme(json, descriptor.name,
                                                descriptor.key);
}

QJsonObject appearanceEnvelope(const QJsonObject &theme)
{
    return {
        {u"format"_s, QString::fromLatin1(APPEARANCE_FORMAT)},
        {u"version"_s, APPEARANCE_VERSION},
        {u"theme"_s, theme},
    };
}

struct ThemeImport {
    ThemeCustomizationProfile profile;
    /// Converted from a Bluzyrino palette.
    bool adapted = false;
    bool localWallpaperRemoved = false;
};

std::optional<ThemeImport> themeFromImport(const QByteArray &bytes,
                                           const QString &fallbackName,
                                           QString *error)
{
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
    {
        *error = u"This is not a supported theme file."_s;
        return std::nullopt;
    }
    auto root = document.object();

    // A theme file straight from the Themes folder.
    if (!root.contains(u"format"_s) && root.value(u"colors"_s).isObject() &&
        customizationProfileFromTheme(root))
    {
        root = appearanceEnvelope(root);
    }
    const auto format = root.value(u"format"_s).toString();
    if (format == QString::fromLatin1(APPEARANCE_FORMAT))
    {
        if (root.value(u"version"_s).toInt() != APPEARANCE_VERSION)
        {
            *error = u"This theme file uses an unsupported version."_s;
            return std::nullopt;
        }
        if (!root.value(u"theme"_s).isObject())
        {
            *error = u"This file does not contain a valid theme."_s;
            return std::nullopt;
        }
        bool localWallpaperRemoved = false;
        const auto theme = makeShareableCustomizedTheme(
            root.value(u"theme"_s).toObject(), &localWallpaperRemoved);
        const auto profile = customizationProfileFromTheme(theme);
        if (!profile)
        {
            *error = u"This file does not contain a valid theme."_s;
            return std::nullopt;
        }
        return ThemeImport{
            .profile = *profile,
            .adapted = false,
            .localWallpaperRemoved = localWallpaperRemoved,
        };
    }

    if (format == u"bluzyrino-theme"_s)
    {
        auto name = root.value(u"name"_s).toString().trimmed();
        if (name.isEmpty())
        {
            name = fallbackName;
        }
        const auto profile = customizationProfileFromBluzyrinoTheme(root, name);
        if (!profile)
        {
            *error = root.value(u"version"_s).toInt() > 2
                         ? u"This Bluzyrino theme is too new to import."_s
                         : u"This Bluzyrino theme is missing a valid color "
                           u"palette."_s;
            return std::nullopt;
        }
        return ThemeImport{
            .profile = *profile,
            .adapted = true,
            .localWallpaperRemoved = false,
        };
    }

    *error = u"This is not a supported theme file."_s;
    return std::nullopt;
}

bool writeThemeFile(const QString &path, const QJsonObject &theme)
{
    QSaveFile file(path);
    const auto contents = QJsonDocument(theme).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) &&
           file.write(contents) == contents.size() && file.commit();
}

QFont previewTimestampFont(QFont chatFont)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    chatFont.setFeature(QFont::Tag("tnum"), 1);
#endif
    return chatFont;
}

template <typename PaintMask>
QImage renderPreviewShadow(const QWidget &widget, const QPainterPath &clip,
                           const QColor &color, int blurRadius,
                           PaintMask &&paintMask)
{
    const qreal dpr = widget.devicePixelRatioF();
    QImage image(
        QSize(qCeil(widget.width() * dpr), qCeil(widget.height() * dpr)),
        QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter shadowPainter(&image);
    shadowPainter.setRenderHint(QPainter::Antialiasing, true);
    shadowPainter.setRenderHint(QPainter::TextAntialiasing, true);
    shadowPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    shadowPainter.setClipPath(clip);
    paintMask(shadowPainter);
    shadowPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    shadowPainter.fillRect(widget.rect(), color);
    shadowPainter.end();

    return blurThemeShadow(std::move(image), qRound(blurRadius * dpr));
}

}  // namespace

/// Picks the part of the background image that stays in view.
class WallpaperFocusWidget final : public QWidget
{
public:
    explicit WallpaperFocusWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setFixedHeight(94);
        this->setMinimumWidth(180);
        this->setCursor(Qt::CrossCursor);
        this->setToolTip(u"Click or drag to set the focus point."_s);
    }

    void setProfile(const ThemeCustomizationProfile &profile)
    {
        const QFileInfo info(profile.wallpaperSource);
        const auto key =
            QStringLiteral("%1:%2")
                .arg(profile.wallpaperSource)
                .arg(info.exists() ? info.lastModified().toMSecsSinceEpoch()
                                   : 0);
        this->profile_ = profile;
        if (key != this->sourceKey_)
        {
            this->sourceKey_ = key;
            this->image_ = this->isVisible() ? loadThemeWallpaper(
                                                   profile.wallpaperSource, 256)
                                             : QImage();
        }
        this->setEnabled(profile.hasWallpaper() &&
                         profile.wallpaperMode != ThemeWallpaperMode::Tile &&
                         (profile.wallpaperMode == ThemeWallpaperMode::Fill ||
                          profile.wallpaperZoom > 100));
        this->update();
    }

    std::function<void(int, int)> focusChanged;

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        this->image_ = loadThemeWallpaper(this->profile_.wallpaperSource, 256);
    }

    void hideEvent(QHideEvent *event) override
    {
        this->image_ = {};
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(this->rect(), this->profile_.chatBackground);
        const QRectF bounds = this->rect().adjusted(1, 1, -1, -1);
        if (this->image_.isNull())
        {
            painter.setPen(this->profile_.mutedText);
            painter.drawText(bounds, Qt::AlignCenter,
                             u"Choose a wallpaper to set its focus"_s);
            painter.setPen(this->profile_.raisedSurface);
            painter.drawRect(bounds);
            this->imageRect_ = {};
            return;
        }

        const auto size = QSizeF(this->image_.size())
                              .scaled(bounds.size(), Qt::KeepAspectRatio);
        this->imageRect_ = QRectF(QPointF(), size);
        this->imageRect_.moveCenter(bounds.center());
        painter.drawImage(this->imageRect_, this->image_);
        painter.fillRect(this->imageRect_, QColor(0, 0, 0, 42));

        const QPointF focus(this->imageRect_.left() +
                                this->imageRect_.width() *
                                    this->profile_.wallpaperFocalX / 100.0,
                            this->imageRect_.top() +
                                this->imageRect_.height() *
                                    this->profile_.wallpaperFocalY / 100.0);
        painter.setPen(QPen(Qt::black, 4));
        painter.drawEllipse(focus, 7, 7);
        painter.setPen(QPen(this->profile_.accent, 2));
        painter.drawEllipse(focus, 7, 7);
        painter.drawLine(focus + QPointF(-11, 0), focus + QPointF(-5, 0));
        painter.drawLine(focus + QPointF(5, 0), focus + QPointF(11, 0));
        painter.drawLine(focus + QPointF(0, -11), focus + QPointF(0, -5));
        painter.drawLine(focus + QPointF(0, 5), focus + QPointF(0, 11));
        painter.setPen(this->profile_.raisedSurface);
        painter.drawRect(bounds);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            this->setFocusFrom(event->position());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons().testFlag(Qt::LeftButton))
        {
            this->setFocusFrom(event->position());
        }
    }

private:
    void setFocusFrom(const QPointF &position)
    {
        if (!this->isEnabled() || this->imageRect_.isEmpty())
        {
            return;
        }
        const int x =
            qRound(std::clamp((position.x() - this->imageRect_.left()) /
                                  this->imageRect_.width(),
                              0.0, 1.0) *
                   100);
        const int y =
            qRound(std::clamp((position.y() - this->imageRect_.top()) /
                                  this->imageRect_.height(),
                              0.0, 1.0) *
                   100);
        if (this->focusChanged)
        {
            this->focusChanged(x, y);
        }
    }

    ThemeCustomizationProfile profile_;
    QImage image_;
    QRectF imageRect_;
    QString sourceKey_;
};

/// Sets where the text shadow falls by dragging.
class ShadowPositionWidget final : public QWidget
{
public:
    explicit ShadowPositionWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setFixedSize(118, 76);
        this->setCursor(Qt::CrossCursor);
        this->setToolTip(u"Drag to position the shadow. Center to reset."_s);
    }

    void setOffset(QPoint offset)
    {
        offset.setX(std::clamp(offset.x(), -8, 8));
        offset.setY(std::clamp(offset.y(), -8, 8));
        if (this->offset_ == offset)
        {
            return;
        }
        this->offset_ = offset;
        this->update();
    }

    QPoint offset() const
    {
        return this->offset_;
    }

    void setShadowColor(QColor color)
    {
        if (!color.isValid() || this->shadowColor_ == color)
        {
            return;
        }
        this->shadowColor_ = color;
        this->update();
    }

    std::function<void(QPoint)> positionChanged;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto bounds = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.fillRect(bounds, this->palette().color(QPalette::Base));
        painter.setPen(this->palette().color(QPalette::Mid));
        painter.drawRect(bounds);
        painter.setPen(
            QPen(this->palette().color(QPalette::Mid), 1, Qt::DashLine));
        painter.drawLine(QPointF(bounds.center().x(), bounds.top() + 7),
                         QPointF(bounds.center().x(), bounds.bottom() - 7));
        painter.drawLine(QPointF(bounds.left() + 9, bounds.center().y()),
                         QPointF(bounds.right() - 9, bounds.center().y()));

        auto sample = makeResolvedFont(this->font(), QFont::Bold);
        sample.setPointSizeF(sample.pointSizeF() + 2);
        painter.setFont(sample);
        const QPointF center = bounds.center() + QPointF(-10, 5);
        const QPointF shadowOffset(this->offset_.x() * 1.8,
                                   this->offset_.y() * 1.8);
        auto shadow = this->shadowColor_;
        shadow.setAlpha(160);
        painter.setPen(shadow);
        painter.drawText(center + shadowOffset, u"Aa"_s);
        painter.setPen(this->palette().color(QPalette::Text));
        painter.drawText(center, u"Aa"_s);

        painter.setPen(Qt::NoPen);
        painter.setBrush(this->palette().color(QPalette::Highlight));
        painter.drawEllipse(this->pointForOffset(this->offset_), 3.5, 3.5);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            this->setFromPoint(event->position());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons().testFlag(Qt::LeftButton))
        {
            this->setFromPoint(event->position());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

private:
    QRectF controlRect() const
    {
        return QRectF(this->rect()).adjusted(10, 9, -10, -9);
    }

    QPointF pointForOffset(QPoint offset) const
    {
        const auto area = this->controlRect();
        return {area.center().x() + area.width() * offset.x() / 16.0,
                area.center().y() + area.height() * offset.y() / 16.0};
    }

    void setFromPoint(QPointF point)
    {
        const auto area = this->controlRect();
        const QPoint next{
            std::clamp(
                qRound((point.x() - area.center().x()) * 16.0 / area.width()),
                -8, 8),
            std::clamp(
                qRound((point.y() - area.center().y()) * 16.0 / area.height()),
                -8, 8),
        };
        if (next == this->offset_)
        {
            return;
        }
        this->offset_ = next;
        this->update();
        if (this->positionChanged)
        {
            this->positionChanged(next);
        }
    }

    QPoint offset_{1, 1};
    QColor shadowColor_{Qt::black};
};

/// A mock-up of a window with the theme being edited.
class ThemePreviewWidget final : public QWidget
{
public:
    QImage wallpaperFrame() const
    {
        return loadThemeWallpaper(this->profile_.wallpaperSource);
    }

    explicit ThemePreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setMinimumSize(290, 420);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        this->setToolTip(u"Live theme preview."_s);
        this->blurTimer_.setSingleShot(true);
        this->blurTimer_.setInterval(90);
        QObject::connect(&this->blurTimer_, &QTimer::timeout, this, [this] {
            this->rebuildWallpaper();
        });

        this->badgeRefreshTimer_.setInterval(120);
        QObject::connect(
            &this->badgeRefreshTimer_, &QTimer::timeout, this, [this] {
                ++this->badgeRefreshAttempts_;
                if (!this->refreshBadgeAssets() ||
                    this->badgeRefreshAttempts_ >= MaxBadgeRefreshAttempts)
                {
                    this->badgeRefreshTimer_.stop();
                }
            });
        if (this->refreshBadgeAssets())
        {
            this->badgeRefreshTimer_.start();
        }
    }

    void setProfile(ThemeCustomizationProfile profile)
    {
        const QFileInfo info(profile.wallpaperSource);
        const auto sourceKey =
            QStringLiteral("%1:%2")
                .arg(profile.wallpaperSource)
                .arg(info.exists() ? info.lastModified().toMSecsSinceEpoch()
                                   : 0);
        profile.clearDisabledFontOverrides();
        this->profile_ = std::move(profile);
        this->generatedTheme_ = buildCustomizedTheme(this->profile_);
        this->shadowPreview_ = {};
        if (this->wallpaperSourceKey_ != sourceKey)
        {
            this->wallpaperSourceKey_ = sourceKey;
            this->wallpaperRenderKey_.clear();
            this->sourceWallpaper_ = {};
            this->wallpaper_ = {};
            if (this->isVisible() && this->profile_.hasWallpaper())
            {
                const auto image =
                    loadThemeWallpaper(this->profile_.wallpaperSource, 768);
                if (!image.isNull())
                {
                    this->sourceWallpaper_ = image;
                    const auto size =
                        QImageReader(this->profile_.wallpaperSource).size();
                    const int fullDimension =
                        std::min(1536, std::max(size.width(), size.height()));
                    this->wallpaperScale_ =
                        fullDimension > 0
                            ? qreal(std::max(image.width(), image.height())) /
                                  fullDimension
                            : 1.0;
                }
            }
        }
        const auto renderKey = QStringLiteral("%1:%2").arg(sourceKey).arg(
            this->profile_.wallpaperBlur);
        if (this->wallpaperRenderKey_ != renderKey)
        {
            this->wallpaperRenderKey_ = renderKey;
            this->blurTimer_.start();
        }
        if (this->refreshBadgeAssets() &&
            !this->badgeRefreshTimer_.isActive() &&
            this->badgeRefreshAttempts_ < MaxBadgeRefreshAttempts)
        {
            this->badgeRefreshTimer_.start();
        }
        this->update();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);

        this->wallpaperSourceKey_.clear();
        this->setProfile(this->profile_);
        if (this->refreshBadgeAssets() && !this->badgeRefreshTimer_.isActive())
        {
            this->badgeRefreshAttempts_ = 0;
            this->badgeRefreshTimer_.start();
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        this->shadowPreview_ = {};
        QWidget::resizeEvent(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        this->blurTimer_.stop();
        this->badgeRefreshTimer_.stop();
        this->sourceWallpaper_ = {};
        this->wallpaper_ = {};
        this->shadowPreview_ = {};
        this->wallpaperSourceKey_.clear();
        this->wallpaperRenderKey_.clear();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF bounds = this->rect().adjusted(1, 1, -1, -1);
        const auto color = [this](std::initializer_list<QString> path,
                                  const QColor &fallback) {
            return nestedColor(this->generatedTheme_, path, fallback);
        };
        const auto windowBackground =
            color({u"colors"_s, u"window"_s, u"background"_s},
                  this->profile_.background);
        const auto regularTab = color({u"colors"_s, u"tabs"_s, u"regular"_s,
                                       u"backgrounds"_s, u"regular"_s},
                                      this->profile_.surface);
        const auto selectedTab = color({u"colors"_s, u"tabs"_s, u"selected"_s,
                                        u"backgrounds"_s, u"regular"_s},
                                       this->profile_.raisedSurface);
        const auto selectedTabLine = color(
            {u"colors"_s, u"tabs"_s, u"selected"_s, u"line"_s, u"regular"_s},
            this->profile_.accent);
        const auto regularTabLine = color(
            {u"colors"_s, u"tabs"_s, u"regular"_s, u"line"_s, u"regular"_s},
            this->profile_.raisedSurface);
        const auto tabDivider =
            color({u"colors"_s, u"tabs"_s, u"dividerLine"_s},
                  this->profile_.raisedSurface);
        const auto headerBackground =
            color({u"colors"_s, u"splits"_s, u"header"_s, u"background"_s},
                  this->profile_.surface);
        const auto focusedHeaderBackground = color(
            {u"colors"_s, u"splits"_s, u"header"_s, u"focusedBackground"_s},
            headerBackground);
        const auto headerBorder = color(
            {u"colors"_s, u"splits"_s, u"header"_s, u"border"_s}, tabDivider);
        const auto focusedHeaderBorder =
            color({u"colors"_s, u"splits"_s, u"header"_s, u"focusedBorder"_s},
                  headerBorder);
        const auto inputBackground =
            color({u"colors"_s, u"splits"_s, u"input"_s, u"background"_s},
                  regularTab);
        const auto messageRegular =
            color({u"colors"_s, u"messages"_s, u"backgrounds"_s, u"regular"_s},
                  this->profile_.chatBackground);
        const auto messageAlternate = color(
            {u"colors"_s, u"messages"_s, u"backgrounds"_s, u"alternate"_s},
            this->profile_.surface);
        const auto interfaceText =
            color({u"colors"_s, u"window"_s, u"text"_s}, this->profile_.text);
        const auto chatText =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"regular"_s},
                  this->profile_.effectiveChatText());
        const auto mutedText = this->profile_.mutedText;
        const auto systemText = color(
            {u"colors"_s, u"messages"_s, u"textColors"_s, u"system"_s},
            this->profile_.systemText.isValid() ? this->profile_.systemText
                                                : this->profile_.mutedText);
        const auto linkText =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"link"_s},
                  this->profile_.accent);
        const auto timestampText =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"timestamp"_s},
                  this->profile_.timestampText.isValid()
                      ? this->profile_.timestampText
                      : systemText);
        const auto accent =
            color({u"colors"_s, u"accent"_s}, this->profile_.accent);

        painter.fillRect(bounds, windowBackground);

        const int radius = this->profile_.cornerRadius();
        const int tabRadius = this->profile_.tabCornerRadius;
        const auto interfaceFont =
            makeResolvedFont(this->profile_.interfaceFontFamily.isEmpty()
                                 ? QStringLiteral(DEFAULT_FONT_FAMILY)
                                 : this->profile_.interfaceFontFamily,
                             this->profile_.interfaceFontSize > 0
                                 ? this->profile_.interfaceFontSize
                                 : 9,
                             QFont::Normal);
        painter.setFont(interfaceFont);

        const QRectF titleBar(bounds.left(), bounds.top(), bounds.width(), 25);
        painter.setPen(interfaceText);
        painter.drawText(titleBar.adjusted(8, 0, -8, 0), Qt::AlignVCenter,
                         Version::instance().fullVersion());

        constexpr qreal TAB_HEIGHT = 28;
        constexpr qreal TAB_SPACER = 1;
        const TabStyle tabStyle = getSettings()->tabStyle;
        const int tabLineWidth = 1;
        const bool compactTabs = tabStyle == TabStyle::Compact;
        const bool showTabCloseButton =
            getSettings()->showTabCloseButton.getValue();
        const QRectF tabStrip(bounds.left(), titleBar.bottom(), bounds.width(),
                              TAB_HEIGHT + 2);
        painter.fillRect(tabStrip, windowBackground);
        const auto drawTab = [&](const QRectF &rect, const QString &text,
                                 bool selected, bool roundStart,
                                 bool roundEnd) {
            auto backgroundRect = rect;
            backgroundRect.setTop(backgroundRect.top() +
                                  (selected ? 0 : tabLineWidth));
            auto shapedRect = backgroundRect;
            if (this->profile_.tabShape == ThemeTabShape::Individual)
            {
                const qreal gap = this->profile_.tabSpacing;
                shapedRect.adjust(gap / 2, 0, -gap / 2, 0);
                roundStart = true;
                roundEnd = true;
            }
            const auto path =
                themeTopTabPath(shapedRect, tabRadius, roundStart, roundEnd);
            painter.fillPath(path, selected ? selectedTab : regularTab);
            painter.setPen(selected ? interfaceText : mutedText);
            const bool showClose = selected && showTabCloseButton;
            const qreal compactDivider = compactTabs ? 1.5 : 1.0;
            auto labelRect =
                rect.adjusted(4 / compactDivider, 0, -4 / compactDivider, 0);
            labelRect.translate(0, selected ? 1 : 2);
            if (showClose)
            {
                labelRect.setRight(labelRect.right() - rect.height() / 2);
            }
            painter.drawText(labelRect, Qt::AlignCenter,
                             QFontMetrics(painter.font())
                                 .elidedText(text, Qt::ElideRight,
                                             qRound(labelRect.width())));
            if (showClose)
            {
                painter.setRenderHint(QPainter::Antialiasing, false);
                const auto closeX = rect.right() - 12;
                const auto closeY = rect.center().y();
                painter.setPen(QPen(interfaceText, 1));
                painter.drawLine(QPointF(closeX - 3, closeY - 3),
                                 QPointF(closeX + 3, closeY + 3));
                painter.drawLine(QPointF(closeX + 3, closeY - 3),
                                 QPointF(closeX - 3, closeY + 3));
                painter.setRenderHint(QPainter::Antialiasing, true);
            }
            if (selected)
            {
                painter.save();
                painter.setClipPath(path);
                painter.fillRect(
                    QRectF(backgroundRect.left(), backgroundRect.top(),
                           backgroundRect.width(), tabLineWidth + 1),
                    selectedTabLine);
                painter.restore();
            }
            else
            {
                painter.save();
                painter.setClipPath(path);
                painter.fillRect(
                    QRectF(backgroundRect.left(), backgroundRect.top(),
                           backgroundRect.width(), tabLineWidth),
                    regularTabLine);
                painter.restore();
            }
        };
        constexpr int TAB_COUNT = 4;
        constexpr int SELECTED_TAB = 0;
        const qreal tabStart = tabStrip.left() + 2;
        const qreal tabAreaWidth = std::max<qreal>(
            0, tabStrip.width() - 4 - TAB_SPACER * (TAB_COUNT - 1));
        const std::array tabNames{u"yusuf7n"_s, u"itsbr0dyy"_s, u"wyydogg"_s,
                                  u"randomkid"_s};
        std::array<qreal, TAB_COUNT> tabWidths{};
        qreal naturalWidth = 0;
        const QFontMetricsF tabMetrics(interfaceFont);
        const qreal compactDivider = compactTabs ? 1.5 : 1.0;
        const qreal tabPadding =
            showTabCloseButton ? 32 / compactDivider : 16 / compactDivider;
        for (int index = 0; index < TAB_COUNT; ++index)
        {
            tabWidths[index] = std::clamp<qreal>(
                tabMetrics.horizontalAdvance(tabNames[index]) + tabPadding,
                TAB_HEIGHT, 150);
            naturalWidth += tabWidths[index];
        }
        if (naturalWidth > tabAreaWidth && naturalWidth > 0)
        {
            const auto scale = tabAreaWidth / naturalWidth;
            for (auto &width : tabWidths)
            {
                width = std::max<qreal>(38, width * scale);
            }

            const qreal selectedTarget =
                std::min(tabMetrics.horizontalAdvance(tabNames[SELECTED_TAB]) +
                             tabPadding,
                         tabAreaWidth - 38 * (TAB_COUNT - 1));
            if (tabWidths[SELECTED_TAB] < selectedTarget)
            {
                const qreal otherWidth =
                    naturalWidth -
                    (tabMetrics.horizontalAdvance(tabNames[SELECTED_TAB]) +
                     tabPadding);
                const qreal remaining = tabAreaWidth - selectedTarget;
                const qreal otherScale =
                    otherWidth > 0 ? remaining / otherWidth : 1;
                for (int index = 0; index < TAB_COUNT; ++index)
                {
                    if (index != SELECTED_TAB)
                    {
                        tabWidths[index] = std::max<qreal>(
                            38, tabWidths[index] * otherScale / scale);
                    }
                }
                tabWidths[SELECTED_TAB] = selectedTarget;
            }
        }
        qreal tabX = tabStart;
        for (int index = 0; index < TAB_COUNT; ++index)
        {
            drawTab(QRectF(tabX, tabStrip.top(), tabWidths[index], TAB_HEIGHT),
                    tabNames[index], index == SELECTED_TAB, index == 0,
                    index == TAB_COUNT - 1);
            tabX += tabWidths[index] + TAB_SPACER;
        }

        painter.fillRect(QRectF(tabStrip.left(), tabStrip.top() + TAB_HEIGHT,
                                tabStrip.width(), 2),
                         tabDivider);

        const QRectF split(bounds.left(), tabStrip.bottom(), bounds.width(),
                           bounds.bottom() - tabStrip.bottom());
        const QRectF channelHeader(split.left(), split.top(), split.width(),
                                   25);
        const QRectF input(split.left(), split.bottom() - 27, split.width(),
                           27);
        const QRectF messages(split.left(), channelHeader.bottom(),
                              split.width(),
                              input.top() - channelHeader.bottom());

        const bool polished =
            this->profile_.foundation == ThemeFoundation::MoltorinoPolished;
        painter.fillRect(channelHeader,
                         polished ? headerBackground : focusedHeaderBackground);
        if (!polished)
        {
            painter.setPen(focusedHeaderBorder);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(channelHeader.adjusted(0.5, 0.5, -0.5, -1.5));
        }
        painter.fillRect(input, inputBackground);
        if (!polished)
        {
            const QColor inputBorder = windowBackground.lightnessF() > 0.5
                                           ? QColor(u"#cccccc"_s)
                                           : QColor(u"#333333"_s);
            painter.setPen(inputBorder);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(input.adjusted(0.5, 0.5, -0.5, -0.5));
        }

        QPainterPath messageClip;
        if (this->profile_.roundChat && radius > 0)
        {
            messageClip.addRoundedRect(messages, radius, radius);
        }
        else
        {
            messageClip.addRect(messages);
        }
        painter.save();
        painter.setClipPath(messageClip);
        painter.fillRect(messages, this->profile_.chatBackground);
        if (!this->wallpaper_.isNull())
        {
            painter.setOpacity(this->profile_.wallpaperOpacity / 100.0);
            const QSizeF imageSize =
                QSizeF(this->wallpaper_.size()) / this->wallpaperScale_;
            if (this->profile_.wallpaperMode == ThemeWallpaperMode::Tile)
            {
                painter.save();
                painter.translate(messages.topLeft());
                const auto zoom =
                    std::clamp(this->profile_.wallpaperZoom, 100, 300) / 100.0;
                painter.scale(zoom, zoom);
                painter.scale(1.0 / this->wallpaperScale_,
                              1.0 / this->wallpaperScale_);
                painter.drawTiledPixmap(
                    QRectF(QPointF(),
                           messages.size() * this->wallpaperScale_ / zoom),
                    this->wallpaper_);
                painter.restore();
            }
            else
            {
                const auto layout = layoutThemeWallpaper(
                    imageSize, messages, this->profile_.wallpaperMode,
                    this->profile_.wallpaperFocalX,
                    this->profile_.wallpaperFocalY,
                    this->profile_.wallpaperZoom);
                painter.drawPixmap(
                    layout.destination, this->wallpaper_,
                    QRectF(layout.source.topLeft() * this->wallpaperScale_,
                           layout.source.size() * this->wallpaperScale_));
            }
            painter.setOpacity(1.0);
            if (this->profile_.wallpaperOpacity > 0)
            {
                auto overlay = this->profile_.wallpaperOverlayColor;
                overlay.setAlphaF(overlay.alphaF() *
                                  this->profile_.wallpaperOverlayOpacity /
                                  100.0);
                painter.fillRect(messages, overlay);
            }
        }

        const auto chatFamily = this->profile_.chatFontFamily.isEmpty()
                                    ? getSettings()->chatFontFamily.getValue()
                                    : this->profile_.chatFontFamily;
        const auto chatFont =
            makeResolvedFont(chatFamily,
                             this->profile_.chatFontSize > 0
                                 ? this->profile_.chatFontSize
                                 : getSettings()->chatFontSize.getValue(),
                             this->profile_.chatFontWeight > 0
                                 ? this->profile_.chatFontWeight
                                 : getSettings()->chatFontWeight.getValue());
        const auto usernameFont =
            makeResolvedFont(this->profile_.usernameFontFamily.isEmpty()
                                 ? chatFamily
                                 : this->profile_.usernameFontFamily,
                             this->profile_.usernameFontSize > 0
                                 ? this->profile_.usernameFontSize
                                 : chatFont.pointSizeF(),
                             this->profile_.usernameFontWeight > 0
                                 ? this->profile_.usernameFontWeight
                                 : getUsernameBoldness());
        const QFontMetricsF chatMetrics(chatFont);
        const QFontMetricsF usernameMetrics(usernameFont);
        const auto timestampFont = previewTimestampFont(chatFont);

        const auto systemFont = chatFont;
        const qreal timestampWidth =
            QFontMetricsF(timestampFont).horizontalAdvance(u"12:34"_s) + 5;
        const qreal lineHeight = std::ceil(std::max(chatMetrics.height(),
                                                    usernameMetrics.height())) +
                                 2;
        const qreal textAscent =
            std::max(chatMetrics.ascent(), usernameMetrics.ascent());
        const qreal badgeSize = std::clamp(lineHeight - 3, 12.0, 18.0);
        const qreal contentLeft = messages.left() + 6;
        const qreal contentRight = messages.right() - 5;
        const qreal wordSpacing = chatMetrics.horizontalAdvance(u" "_s);
        const bool showAlternates =
            this->profile_.useThemeMessageRows
                ? this->profile_.alternateMessageRows
                : getSettings()->alternateMessages.getValue();

        struct PreviewMessage {
            QString username;
            QString message;
            QColor usernameColor;
            int paint = 0;
            std::array<ImagePtr, 3> badges{};
            int badgeCount = 0;
            bool highlighted = false;
            QString linkWord;
            bool system = false;
        };
        const std::array<PreviewMessage, 7> chatMessages{
            PreviewMessage{
                u"viewer_one"_s,
                u"nah i finally got this theme right"_s,
                linkText,
                1,
                {this->moderatorBadge_, this->subscriberBadge_,
                 this->partnerBadge_},
                3,
            },
            PreviewMessage{
                u"itsbr0dyy"_s,
                u"lemme see what you did"_s,
                QColor(u"#63e6be"_s),
                2,
                {this->subscriberBadge_, this->vipBadge_, nullptr},
                2,
            },
            PreviewMessage{
                u"randomkid"_s,
                u"wait where yall get that theme"_s,
                QColor(u"#b998ff"_s),
                3,
                {this->vipBadge_, nullptr, nullptr},
                1,
            },
            PreviewMessage{
                u"wyydogg"_s,
                u"made it myself https://example.com"_s,
                QColor(u"#66d9ef"_s),
                0,
                {this->subGifterBadge_, nullptr, nullptr},
                1,
                false,
                u"https://example.com"_s,
            },
            PreviewMessage{
                u"yusuf7n"_s,
                u"okay wait the pink kinda hard"_s,
                QColor(u"#f2b45b"_s),
                0,
                {this->broadcasterBadge_, this->partnerBadge_, nullptr},
                2,
            },
            PreviewMessage{
                u"itsbr0dyy"_s,
                u"@viewer_one send the theme when you done"_s,
                QColor(u"#63e6be"_s),
                2,
                {this->subscriberBadge_, this->vipBadge_, nullptr},
                2,
                true,
            },
            PreviewMessage{
                {},
                u"Theme saved. New messages use this look."_s,
                systemText,
                0,
                {},
                0,
                false,
                {},
                true,
            },
        };

        struct PreviewPiece {
            enum class Kind : uint8_t {
                Text,
                Image,
            };

            Kind kind = Kind::Text;
            QPointF baseline;
            QRectF imageRect;
            QString text;
            QFont font;
            QBrush brush;
            ImagePtr image;
        };
        struct PreviewRow {
            QRectF rect;
            int sourceIndex = 0;
            bool highlighted = false;
            std::vector<PreviewPiece> pieces;
        };

        std::vector<PreviewRow> rows;
        rows.reserve(chatMessages.size());
        qreal rowTop = messages.top() + 3;
        for (int index = 0; index < int(chatMessages.size()); ++index)
        {
            if (rowTop >= messages.bottom())
            {
                break;
            }

            const auto &message = chatMessages[index];
            PreviewRow row;
            row.sourceIndex = index;
            row.highlighted = message.highlighted;
            row.pieces.reserve(size_t(message.badgeCount) + 12);

            int line = 0;
            qreal x = contentLeft;
            const auto lineTop = [&] {
                return rowTop + line * lineHeight;
            };
            const auto baseline = [&] {
                return QPointF(x, lineTop() + textAscent);
            };
            const auto nextLine = [&] {
                ++line;
                x = contentLeft;
            };

            PreviewPiece timestampPiece;
            timestampPiece.baseline = baseline();
            timestampPiece.text = index < 3 ? u"12:34"_s : u"12:35"_s;
            timestampPiece.font = timestampFont;
            timestampPiece.brush = QBrush(timestampText);
            row.pieces.push_back(std::move(timestampPiece));
            x += timestampWidth;

            for (int badgeIndex = 0;
                 !message.system && badgeIndex < message.badgeCount;
                 ++badgeIndex)
            {
                const auto &badge = message.badges[badgeIndex];
                if (!badge || badge->isEmpty())
                {
                    continue;
                }
                PreviewPiece piece;
                piece.kind = PreviewPiece::Kind::Image;
                piece.image = badge;
                piece.imageRect =
                    QRectF(x, lineTop() + (lineHeight - badgeSize) / 2,
                           badgeSize, badgeSize);
                row.pieces.push_back(std::move(piece));
                x += badgeSize + 2;
            }

            if (!message.system)
            {
                const auto username = message.username + u":"_s;
                const qreal usernameWidth =
                    usernameMetrics.horizontalAdvance(username);
                QBrush usernameBrush(message.usernameColor);
                if (message.paint != 0)
                {
                    QLinearGradient paint(x, 0, x + usernameWidth, 0);
                    if (message.paint == 1)
                    {
                        paint.setColorAt(0, QColor(u"#ffd86f"_s));
                        paint.setColorAt(0.52, linkText);
                        paint.setColorAt(1, QColor(u"#ff8c42"_s));
                    }
                    else if (message.paint == 2)
                    {
                        paint.setColorAt(0, QColor(u"#62d8ff"_s));
                        paint.setColorAt(0.5, QColor(u"#d56dff"_s));
                        paint.setColorAt(1, QColor(u"#ff6b91"_s));
                    }
                    else
                    {
                        paint.setColorAt(0, QColor(u"#8effa0"_s));
                        paint.setColorAt(0.5, QColor(u"#54d8ff"_s));
                        paint.setColorAt(1, QColor(u"#b998ff"_s));
                    }
                    usernameBrush = QBrush(paint);
                }
                PreviewPiece usernamePiece;
                usernamePiece.baseline = baseline();
                usernamePiece.text = username;
                usernamePiece.font = usernameFont;
                usernamePiece.brush = usernameBrush;
                row.pieces.push_back(std::move(usernamePiece));
                x += usernameWidth + wordSpacing;
            }

            const auto words = message.message.split(u' ', Qt::SkipEmptyParts);
            for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex)
            {
                const auto &word = words[wordIndex];
                const qreal width = chatMetrics.horizontalAdvance(word);
                if (x + width > contentRight && x > contentLeft)
                {
                    nextLine();
                }
                PreviewPiece wordPiece;
                wordPiece.baseline = baseline();
                wordPiece.text = word;
                wordPiece.font = message.system ? systemFont : chatFont;
                auto wordColor = chatText;
                if (message.system)
                {
                    wordColor = systemText;
                }
                else if (word == message.linkWord)
                {
                    wordColor = linkText;
                }
                wordPiece.brush = QBrush(wordColor);
                row.pieces.push_back(std::move(wordPiece));
                x += width;
                if (wordIndex + 1 < words.size())
                {
                    x += wordSpacing;
                }
            }

            const qreal rowHeight = (line + 1) * lineHeight + 2;
            row.rect = QRectF(messages.left(), rowTop - 1, messages.width(),
                              rowHeight);
            rows.push_back(std::move(row));
            rowTop += rowHeight;
        }

        enum class RowPass : uint8_t {
            Background,
            Shadow,
            Content,
        };
        const auto paintRows = [&](QPainter &target, RowPass pass) {
            target.setRenderHint(QPainter::SmoothPixmapTransform, true);
            for (const auto &row : rows)
            {
                if (pass == RowPass::Background)
                {
                    auto rowBackground =
                        showAlternates && row.sourceIndex % 2 == 1
                            ? messageAlternate
                            : messageRegular;
                    if (row.highlighted)
                    {
                        rowBackground = blendThemeHighlight(
                            rowBackground,
                            *ColorProvider::instance().color(
                                ColorType::SelfHighlight),
                            this->profile_.highlightOpacityAdjustment);
                    }
                    target.fillRect(row.rect, rowBackground);
                    continue;
                }

                const QPointF offset =
                    pass == RowPass::Shadow
                        ? this->profile_.messageShadowOffset()
                        : QPointF();
                for (const auto &piece : row.pieces)
                {
                    if (piece.kind == PreviewPiece::Kind::Image)
                    {
                        if (const auto pixmap = piece.image->pixmapOrLoad())
                        {
                            target.drawPixmap(
                                piece.imageRect.translated(offset), *pixmap,
                                pixmap->rect());
                        }
                        continue;
                    }
                    target.setFont(piece.font);
                    target.setPen(pass == RowPass::Shadow
                                      ? QPen(Qt::black)
                                      : QPen(piece.brush, 1));
                    target.drawText(piece.baseline + offset, piece.text);
                }
            }
        };

        paintRows(painter, RowPass::Background);
        if (this->profile_.messageShadow &&
            this->profile_.messageShadowOpacity > 0)
        {
            const qreal dpr = this->devicePixelRatioF();
            const QSize shadowSize(qCeil(this->width() * dpr),
                                   qCeil(this->height() * dpr));
            if (this->shadowPreview_.size() != shadowSize ||
                !qFuzzyCompare(this->shadowPreview_.devicePixelRatio(), dpr))
            {
                this->shadowPreview_ = renderPreviewShadow(
                    *this, messageClip, this->profile_.messageShadowColor,
                    this->profile_.messageShadowBlur,
                    [&paintRows](QPainter &shadowPainter) {
                        paintRows(shadowPainter, RowPass::Shadow);
                    });
            }
            painter.save();
            painter.setOpacity(this->profile_.messageShadowOpacity / 100.0);
            painter.drawImage(QPointF(), this->shadowPreview_);
            painter.restore();
        }
        paintRows(painter, RowPass::Content);
        painter.restore();

        if (this->profile_.roundChat && radius > 0)
        {
            paintThemeChatFrame(
                painter, messages, radius,
                this->profile_.chatBorder
                    ? std::clamp(this->profile_.chatBorderWidth, 1, 4)
                    : 0,
                polished ? headerBackground : focusedHeaderBackground,
                inputBackground, this->profile_.chatBorderColor,
                this->profile_.chatBorder ? this->profile_.chatBorderOpacity
                                          : 0);
        }

        painter.setFont(interfaceFont);
        painter.setPen(color({u"colors"_s, u"splits"_s, u"header"_s,
                              polished ? u"text"_s : u"focusedText"_s},
                             interfaceText));
        painter.drawText(channelHeader.adjusted(8, 0, -8, 0),
                         Qt::AlignVCenter | Qt::AlignHCenter,
                         u"yusuf7n (live)"_s);
        painter.setPen(color(
            {u"colors"_s, u"messages"_s, u"textColors"_s, u"chatPlaceholder"_s},
            mutedText));
        painter.drawText(input.adjusted(8, 0, -8, 0), Qt::AlignVCenter,
                         u"Send message..."_s);

        painter.setPen(QPen(tabDivider));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bounds);
    }

private:
    bool refreshBadgeAssets()
    {
        bool pending = false;
        bool visualChanged = false;
        size_t badgeIndex = 0;
        const auto load = [&](ImagePtr &target, const QString &set,
                              const QString &version) {
            if (!target)
            {
                if (const auto badge =
                        getApp()->getTwitchBadges()->badge(set, version))
                {
                    target = (*badge)->images.getImage1();
                }
            }
            qint64 pixmapKey = 0;
            if (target)
            {
                if (const auto pixmap = target->pixmapOrLoad())
                {
                    pixmapKey = pixmap->cacheKey();
                }
                else
                {
                    pending = true;
                }
            }
            else
            {
                pending = true;
            }
            if (this->badgePixmapKeys_[badgeIndex] != pixmapKey)
            {
                this->badgePixmapKeys_[badgeIndex] = pixmapKey;
                visualChanged = true;
            }
            ++badgeIndex;
        };
        load(this->moderatorBadge_, u"moderator"_s, u"1"_s);
        load(this->subscriberBadge_, u"subscriber"_s, u"0"_s);
        load(this->vipBadge_, u"vip"_s, u"1"_s);
        load(this->partnerBadge_, u"partner"_s, u"1"_s);
        load(this->broadcasterBadge_, u"broadcaster"_s, u"1"_s);
        load(this->subGifterBadge_, u"sub-gifter"_s, u"1"_s);
        if (visualChanged)
        {
            this->shadowPreview_ = {};
            this->update();
        }
        return pending;
    }

    void rebuildWallpaper()
    {
        this->wallpaper_ = {};
        if (!this->sourceWallpaper_.isNull())
        {
            this->wallpaper_ = QPixmap::fromImage(blurThemeWallpaper(
                this->sourceWallpaper_,
                qRound(this->profile_.wallpaperBlur * this->wallpaperScale_)));
        }
        this->update();
    }

    ThemeCustomizationProfile profile_;
    QJsonObject generatedTheme_;
    QImage sourceWallpaper_;
    qreal wallpaperScale_ = 1.0;
    QImage shadowPreview_;
    QPixmap wallpaper_;
    QString wallpaperSourceKey_;
    QString wallpaperRenderKey_;
    QTimer blurTimer_;
    QTimer badgeRefreshTimer_;
    static constexpr int MaxBadgeRefreshAttempts = 50;
    int badgeRefreshAttempts_ = 0;
    std::array<qint64, 6> badgePixmapKeys_{};
    ImagePtr moderatorBadge_;
    ImagePtr subscriberBadge_;
    ImagePtr vipBadge_;
    ImagePtr partnerBadge_;
    ImagePtr broadcasterBadge_;
    ImagePtr subGifterBadge_;
};

/// One line of chat in the fonts of the theme being edited.
class TypographyPreviewWidget final : public QWidget
{
public:
    explicit TypographyPreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setMinimumHeight(68);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        this->setToolTip(u"Preview the fonts and shadow."_s);
    }

    void setProfile(ThemeCustomizationProfile profile)
    {
        profile.clearDisabledFontOverrides();
        this->profile_ = std::move(profile);
        this->generatedTheme_ = buildCustomizedTheme(this->profile_);
        this->shadowPreview_ = {};
        this->updateGeometry();
        this->update();
    }

    QSize sizeHint() const override
    {
        const auto chatFont = this->chatPreviewFont();
        const auto usernameFont = this->usernamePreviewFont(chatFont);
        const QFontMetricsF chatMetrics(chatFont);
        const QFontMetricsF usernameMetrics(usernameFont);
        const int contentHeight =
            qCeil(std::max(chatMetrics.height(), usernameMetrics.height())) +
            24;
        return {300, std::max(68, contentHeight)};
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        this->shadowPreview_ = {};
        QWidget::resizeEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        const auto color = [this](std::initializer_list<QString> path,
                                  const QColor &fallback) {
            return nestedColor(this->generatedTheme_, path, fallback);
        };
        const auto background =
            color({u"colors"_s, u"messages"_s, u"backgrounds"_s, u"regular"_s},
                  this->profile_.chatBackground);
        const auto text =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"regular"_s},
                  this->profile_.effectiveChatText());
        const auto usernameColor =
            color({u"colors"_s, u"messages"_s, u"textColors"_s, u"link"_s},
                  this->profile_.effectiveChatText());
        const auto frame =
            color({u"colors"_s, u"splits"_s, u"input"_s, u"border"_s},
                  this->profile_.raisedSurface);

        const QRectF bounds = this->rect().adjusted(1, 1, -1, -1);
        const int radius =
            this->profile_.roundChat ? this->profile_.cornerRadius() : 0;
        QPainterPath clip;
        if (radius > 0)
        {
            clip.addRoundedRect(bounds, radius, radius);
        }
        else
        {
            clip.addRect(bounds);
        }
        painter.setClipPath(clip);
        painter.fillRect(bounds, background);

        const auto chatFont = this->chatPreviewFont();
        const auto usernameFont = this->usernamePreviewFont(chatFont);
        const QFontMetricsF chatMetrics(chatFont);
        const QFontMetricsF usernameMetrics(usernameFont);

        const qreal horizontalPadding = 14;
        const qreal contentWidth =
            std::max<qreal>(1, bounds.width() - horizontalPadding * 2);
        const QString username = u"viewer_one:"_s;
        const QString message = u"this theme feels like mine"_s;
        const qreal usernameWidth = usernameMetrics.horizontalAdvance(username);
        const qreal gap = chatMetrics.horizontalAdvance(u" "_s);
        const qreal inlineOffset = usernameWidth + gap;
        const qreal messageWidth = chatMetrics.horizontalAdvance(message);
        const qreal inlineWidth = contentWidth - inlineOffset;
        const bool startMessageOnNextLine = inlineWidth < messageWidth;
        const qreal rowWidth = inlineOffset + messageWidth;
        const qreal rowLeft =
            !startMessageOnNextLine
                ? bounds.left() +
                      std::max<qreal>(horizontalPadding,
                                      (bounds.width() - rowWidth) / 2)
                : bounds.left() + horizontalPadding;
        const qreal messageX =
            rowLeft + (startMessageOnNextLine ? 0 : inlineOffset);

        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        QTextLayout messageLayout(message, chatFont);
        messageLayout.setTextOption(option);
        messageLayout.beginLayout();
        qreal messageHeight = 0;
        qreal firstLineAscent = chatMetrics.ascent();
        int lineIndex = 0;
        while (true)
        {
            auto line = messageLayout.createLine();
            if (!line.isValid())
            {
                break;
            }
            const bool firstLine = lineIndex++ == 0;
            const qreal width = firstLine && !startMessageOnNextLine
                                    ? std::max<qreal>(1, inlineWidth)
                                    : contentWidth;
            line.setLineWidth(width);
            line.setPosition(
                QPointF(firstLine || startMessageOnNextLine ? 0 : -inlineOffset,
                        messageHeight));
            if (firstLine)
            {
                firstLineAscent = line.ascent();
            }
            messageHeight += line.height();
        }
        messageLayout.endLayout();

        const qreal usernameTop =
            startMessageOnNextLine ? usernameMetrics.height() + 3 : 0;
        const qreal aboveBaseline =
            std::max(usernameMetrics.ascent(),
                     startMessageOnNextLine ? 0.0 : firstLineAscent);
        const qreal belowBaseline = std::max(
            usernameMetrics.descent(),
            startMessageOnNextLine
                ? usernameTop + messageHeight - usernameMetrics.ascent()
                : messageHeight - firstLineAscent);
        const qreal baseline =
            bounds.center().y() + (aboveBaseline - belowBaseline) / 2;
        const QPointF usernamePosition(rowLeft, baseline);
        const qreal messageTop = startMessageOnNextLine
                                     ? baseline + usernameMetrics.descent() + 3
                                     : baseline - firstLineAscent;
        const QPointF messagePosition(messageX, messageTop);

        const auto paintSample = [&](QPainter &target, bool shadow) {
            target.setFont(usernameFont);
            target.setPen(shadow ? QColor(Qt::black) : usernameColor);
            target.drawText(usernamePosition, username);
            target.setFont(chatFont);
            target.setPen(shadow ? QColor(Qt::black) : text);
            messageLayout.draw(&target, messagePosition);
        };

        if (this->profile_.messageShadow &&
            this->profile_.messageShadowOpacity > 0)
        {
            const qreal dpr = this->devicePixelRatioF();
            const QSize shadowSize(qCeil(this->width() * dpr),
                                   qCeil(this->height() * dpr));
            if (this->shadowPreview_.size() != shadowSize ||
                !qFuzzyCompare(this->shadowPreview_.devicePixelRatio(), dpr))
            {
                this->shadowPreview_ = renderPreviewShadow(
                    *this, clip, this->profile_.messageShadowColor,
                    this->profile_.messageShadowBlur,
                    [this, &paintSample](QPainter &shadowPainter) {
                        shadowPainter.translate(
                            this->profile_.messageShadowOffset());
                        paintSample(shadowPainter, true);
                    });
            }
            painter.save();
            painter.setOpacity(this->profile_.messageShadowOpacity / 100.0);
            painter.drawImage(QPointF(), this->shadowPreview_);
            painter.restore();
        }
        paintSample(painter, false);

        painter.setClipping(false);
        painter.setPen(QPen(frame, 1));
        painter.setBrush(Qt::NoBrush);
        if (radius > 0)
        {
            painter.drawRoundedRect(bounds, radius, radius);
        }
        else
        {
            painter.drawRect(bounds);
        }
    }

private:
    QFont chatPreviewFont() const
    {
        const auto family = this->profile_.chatFontFamily.isEmpty()
                                ? getSettings()->chatFontFamily.getValue()
                                : this->profile_.chatFontFamily;
        const auto weight = this->profile_.chatFontWeight > 0
                                ? this->profile_.chatFontWeight
                                : getSettings()->chatFontWeight.getValue();

        return makeResolvedFont(family, 14, weight);
    }

    QFont usernamePreviewFont(const QFont &chatFont) const
    {
        const auto family = this->profile_.usernameFontFamily.isEmpty()
                                ? chatFont.family()
                                : this->profile_.usernameFontFamily;
        const auto weight = this->profile_.usernameFontWeight > 0
                                ? this->profile_.usernameFontWeight
                                : getUsernameBoldness();
        return makeResolvedFont(family, chatFont.pointSizeF(), weight);
    }

    ThemeCustomizationProfile profile_;
    QJsonObject generatedTheme_;
    QImage shadowPreview_;
};

CustomizationPage::CustomizationPage()
{
    this->buildUi();
    this->reloadProfiles(getTheme()->themeName.getValue());
    this->managedConnections_.managedConnect(getTheme()->updated, [this] {
        if (!this->isVisible())
        {
            return;
        }
        if (this->selectedCustom_ && this->profile_ != this->savedProfile_)
        {
            this->updateControlState();
            return;
        }
        this->reloadProfiles(this->selectedKey_.isEmpty()
                                 ? getTheme()->themeName.getValue()
                                 : this->selectedKey_);
    });
    getSettings()->alternateMessages.connect(
        [this] {
            this->preview_->update();
        },
        this->managedConnections_, false);
}

void CustomizationPage::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(8);

    auto *profileRow = new QHBoxLayout;
    profileRow->setSpacing(6);
    profileRow->addWidget(new QLabel(u"Theme:"_s));
    this->profileBox_ = new QComboBox;
    this->profileBox_->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Preferred);
    profileRow->addWidget(this->profileBox_, 1);
    this->profileKind_ = new QLabel;
    this->profileKind_->setMinimumWidth(56);
    profileRow->addWidget(this->profileKind_);
    this->useButton_ = new QPushButton(u"Apply theme"_s);
    this->duplicateButton_ = new QPushButton(u"Customize"_s);
    this->renameButton_ = new QPushButton(u"Rename"_s);
    this->deleteButton_ = new QPushButton(u"Delete"_s);
    profileRow->addWidget(this->useButton_);
    profileRow->addWidget(this->duplicateButton_);
    profileRow->addWidget(this->renameButton_);
    profileRow->addWidget(this->deleteButton_);
    outer->addLayout(profileRow);

    auto *workspace = new QSplitter(Qt::Horizontal, this);
    workspace->setChildrenCollapsible(false);
    workspace->setHandleWidth(7);
    this->preview_ = new ThemePreviewWidget;
    this->preview_->setMinimumWidth(200);
    workspace->addWidget(this->preview_);

    auto *tabs = new QTabWidget;
    tabs->setMinimumWidth(380);
    tabs->addTab(this->buildColorsTab(), u"Colors"_s);
    tabs->addTab(this->buildChatTab(), u"Chat"_s);
    tabs->addTab(this->buildInterfaceTab(), u"Interface"_s);
    tabs->addTab(this->buildBackgroundTab(), u"Background"_s);
    tabs->addTab(this->buildTextTab(), u"Text"_s);
    tabs->addTab(this->buildShareTab(), u"Import and export"_s);
    workspace->addWidget(tabs);
    workspace->setStretchFactor(0, 2);
    workspace->setStretchFactor(1, 3);
    workspace->setSizes({260, 520});
    outer->addWidget(workspace, 1);

    auto *footer = new QHBoxLayout;
    this->status_ = new QLabel;
    this->status_->setTextFormat(Qt::PlainText);
    footer->addWidget(this->status_, 1);
    this->resetButton_ = new QPushButton(u"Revert changes"_s);
    this->saveButton_ = new QPushButton(u"Save and apply"_s);
    footer->addWidget(this->resetButton_);
    footer->addWidget(this->saveButton_);
    outer->addLayout(footer);

    QObject::connect(
        this->profileBox_, QOverload<int>::of(&QComboBox::activated), this,
        [this] {
            const auto requestedKey = this->profileBox_->currentData();
            if (!this->confirmProfileChange())
            {
                const QSignalBlocker blocker(this->profileBox_);
                this->profileBox_->setCurrentIndex(
                    this->profileBox_->findData(this->selectedKey_));
                return;
            }

            // Saving may have rebuilt the list.
            this->profileBox_->setCurrentIndex(
                this->profileBox_->findData(requestedKey));
            this->loadSelectedProfile();
        });
    QObject::connect(this->useButton_, &QPushButton::clicked, this, [this] {
        this->applySelectedTheme();
    });
    QObject::connect(this->duplicateButton_, &QPushButton::clicked, this,
                     [this] {
                         this->duplicateProfile();
                     });
    QObject::connect(this->renameButton_, &QPushButton::clicked, this, [this] {
        this->renameProfile();
    });
    QObject::connect(this->deleteButton_, &QPushButton::clicked, this, [this] {
        this->deleteProfile();
    });
    QObject::connect(this->saveButton_, &QPushButton::clicked, this, [this] {
        this->saveCustomProfile(true);
    });
    QObject::connect(this->resetButton_, &QPushButton::clicked, this, [this] {
        this->resetDraft();
    });
}

QWidget *CustomizationPage::buildColorsTab()
{
    auto *page = new QWidget;
    this->colorsEditor_ = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto *description = new QLabel(u"Choose your app colors."_s);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *surfaces = new QGroupBox(u"Surfaces"_s);
    auto *surfaceGrid = new QGridLayout(surfaces);
    surfaceGrid->setHorizontalSpacing(14);
    surfaceGrid->setVerticalSpacing(6);
    auto *details = new QGroupBox(u"Text colors"_s);
    auto *detailGrid = new QGridLayout(details);
    detailGrid->setHorizontalSpacing(14);
    detailGrid->setVerticalSpacing(6);
    struct Definition {
        QString role;
        QString label;
        QString description;
        QColor ThemeCustomizationProfile::*member;
    };
    const std::array definitions{
        Definition{u"background"_s, u"Background"_s,
                   u"Window background behind tabs and panels."_s,
                   &ThemeCustomizationProfile::background},
        Definition{u"chatBackground"_s, u"Chat background"_s,
                   u"Background behind chat messages."_s,
                   &ThemeCustomizationProfile::chatBackground},
        Definition{u"surface"_s, u"Surface"_s, u"Tabs, menus, and controls."_s,
                   &ThemeCustomizationProfile::surface},
        Definition{u"raisedSurface"_s, u"Raised surface"_s,
                   u"Selected tabs, inputs, and raised panels."_s,
                   &ThemeCustomizationProfile::raisedSurface},
        Definition{u"text"_s, u"Text"_s, u"Main interface text."_s,
                   &ThemeCustomizationProfile::text},
        Definition{u"chatText"_s, u"Chat text"_s, u"Regular chat messages."_s,
                   &ThemeCustomizationProfile::chatText},
        Definition{u"mutedText"_s, u"Muted text"_s,
                   u"Secondary interface text."_s,
                   &ThemeCustomizationProfile::mutedText},
        Definition{u"systemText"_s, u"System messages"_s,
                   u"Connection and moderation notices."_s,
                   &ThemeCustomizationProfile::systemText},
        Definition{u"timestampText"_s, u"Timestamps"_s,
                   u"Message timestamps."_s,
                   &ThemeCustomizationProfile::timestampText},
        Definition{u"accent"_s, u"Accent"_s,
                   u"Active tabs, links, and focused controls."_s,
                   &ThemeCustomizationProfile::accent},
    };
    for (int index = 0; index < static_cast<int>(definitions.size()); ++index)
    {
        const auto &definition = definitions.at(index);
        const bool surfaceColor = index < 4;
        const int localIndex = surfaceColor ? index : index - 4;
        const int column = localIndex % 2;
        const int row = localIndex / 2;
        auto *container = new QWidget;
        auto *rowLayout = new QHBoxLayout(container);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(5);
        auto *label = new QLabel(definition.label);
        label->setMinimumWidth(102);
        auto *button = new QPushButton;
        button->setFixedWidth(34);
        auto *input = new QLineEdit;
        input->setMaxLength(9);
        container->setToolTip(definition.description);
        button->setToolTip(definition.description);
        input->setToolTip(definition.description);
        rowLayout->addWidget(label);
        rowLayout->addWidget(button);
        rowLayout->addWidget(input, 1);
        (surfaceColor ? surfaceGrid : detailGrid)
            ->addWidget(container, row, column);
        this->colorControls_.append({button, input, definition.member});
        QObject::connect(
            button, &QPushButton::clicked, this,
            [this, member = definition.member, role = definition.role] {
                auto *picker =
                    new ColorPickerDialog(this->profile_.*member, this);
                QObject::connect(
                    picker, &ColorPickerDialog::colorConfirmed, this,
                    [this, member, role, generation = this->profileGeneration_](
                        const QColor &color) {
                        if (generation != this->profileGeneration_)
                        {
                            return;
                        }
                        this->profile_.*member = color;
                        this->markColorEdited(role);
                        this->loadProfileIntoControls();
                    });
                picker->show();
            });
        QObject::connect(input, &QLineEdit::editingFinished, this,
                         [this, input, role = definition.role] {
                             if (QColor(input->text().trimmed()).isValid())
                             {
                                 this->markColorEdited(role);
                             }
                             this->readControlsIntoProfile();
                             this->loadProfileIntoControls();
                         });
    }
    this->chatTextFollowsText_ = new QCheckBox(u"Use Text for chat messages"_s);
    this->chatTextFollowsText_->setToolTip(
        u"Use the same color for chat and interface text."_s);
    detailGrid->addWidget(this->chatTextFollowsText_, 3, 0, 1, 2);
    QObject::connect(this->chatTextFollowsText_, &QCheckBox::toggled, this,
                     [this](bool linked) {
                         if (this->loadingControls_)
                         {
                             return;
                         }
                         this->profile_.separateChatText = !linked;
                         if (linked)
                         {
                             this->profile_.chatText = this->profile_.text;
                             this->editedColorRoles_.remove(u"chatText"_s);
                         }
                         this->loadProfileIntoControls();
                     });
    layout->addWidget(surfaces);
    layout->addWidget(details);

    auto *paletteHelper = new QGroupBox(u"Palette helper"_s);
    auto *paletteLayout = new QHBoxLayout(paletteHelper);
    paletteLayout->setContentsMargins(8, 5, 8, 5);
    auto *paletteHint = new QLabel(u"Keep some colors and rebuild the rest."_s);
    paletteHint->setWordWrap(true);
    paletteLayout->addWidget(paletteHint, 1);
    auto *buildPaletteButton = new QPushButton(u"Build palette…"_s);
    buildPaletteButton->setToolTip(u"Build around your chosen colors."_s);
    paletteLayout->addWidget(buildPaletteButton);
    layout->addWidget(paletteHelper);

    auto *behavior = new QGroupBox(u"Color balance"_s);
    auto *behaviorForm = new QFormLayout(behavior);
    behaviorForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *panelContrastRow = makeSlider(0, 100, &this->panelContrast_, u"%"_s);
    this->panelContrast_->setToolTip(u"Adjusts contrast between panels."_s);
    panelContrastRow->setToolTip(this->panelContrast_->toolTip());
    this->accentStrength_ = new QComboBox;
    this->accentStrength_->addItems({u"Subtle"_s, u"Balanced"_s, u"Bold"_s});
    this->accentStrength_->setToolTip(
        u"How strongly controls use the accent color."_s);
    behaviorForm->addRow(u"Panel contrast:"_s, panelContrastRow);
    behaviorForm->addRow(u"Accent use:"_s, this->accentStrength_);
    behaviorForm->addRow(
        u"Inactive tabs:"_s,
        makeSlider(0, 100, &this->inactiveTabContrast_, u"%"_s));
    this->inactiveTabContrast_->setToolTip(
        u"Contrast between inactive tabs and the window."_s);
    behaviorForm->addRow(
        u"Channel bar:"_s,
        makeSlider(0, 100, &this->channelBarContrast_, u"%"_s));
    this->channelBarContrast_->setToolTip(
        u"Contrast between the channel bar and chat."_s);
    layout->addWidget(behavior);

    this->readability_ = new QLabel;
    this->readability_->setWordWrap(true);
    layout->addWidget(this->readability_);
    layout->addStretch(1);

    const auto update = [this] {
        if (!this->loadingControls_)
        {
            this->readControlsIntoProfile();
        }
    };
    for (auto *slider : {this->panelContrast_, this->inactiveTabContrast_,
                         this->channelBarContrast_})
    {
        QObject::connect(slider, &QSlider::valueChanged, this, update);
    }
    QObject::connect(this->accentStrength_, &QComboBox::currentIndexChanged,
                     this, update);
    QObject::connect(buildPaletteButton, &QPushButton::clicked, this, [this] {
        this->buildPalette();
    });
    return page;
}

QWidget *CustomizationPage::makeColorControl(
    const QString &tooltip, QColor ThemeCustomizationProfile::*member)
{
    auto *container = new QWidget;
    auto *rowLayout = new QHBoxLayout(container);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(5);
    auto *button = new QPushButton;
    button->setFixedWidth(34);
    auto *input = new QLineEdit;
    input->setMaxLength(9);
    input->setMaximumWidth(110);
    container->setToolTip(tooltip);
    button->setToolTip(tooltip);
    input->setToolTip(tooltip);
    rowLayout->addWidget(button);
    rowLayout->addWidget(input);
    rowLayout->addStretch(1);
    this->colorControls_.append({button, input, member});
    QObject::connect(button, &QPushButton::clicked, this, [this, member] {
        auto *picker = new ColorPickerDialog(this->profile_.*member, this);
        QObject::connect(picker, &ColorPickerDialog::colorConfirmed, this,
                         [this, member, generation = this->profileGeneration_](
                             const QColor &color) {
                             if (generation != this->profileGeneration_)
                             {
                                 return;
                             }
                             this->profile_.*member = color;
                             this->loadProfileIntoControls();
                         });
        picker->show();
    });
    QObject::connect(input, &QLineEdit::editingFinished, this, [this] {
        this->readControlsIntoProfile();
        this->loadProfileIntoControls();
    });
    return container;
}

QWidget *CustomizationPage::buildChatTab()
{
    auto *page = new QWidget;
    this->chatEditor_ = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *messages = new QGroupBox(u"Messages"_s);
    auto *messageForm = new QFormLayout(messages);
    messageForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->alternateMessageRows_ =
        new QCheckBox(u"Alternate message backgrounds"_s);
    this->alternateMessageRows_->setToolTip(
        u"With a theme from this page, this replaces the \"Alternate "
        u"background color\" setting."_s);
    messageForm->addRow(this->alternateMessageRows_);
    messageForm->addRow(
        u"Row strength:"_s,
        makeSlider(0, 100, &this->alternateMessageOpacity_, u"%"_s));
    messageForm->addRow(
        u"Row contrast:"_s,
        makeSlider(0, 100, &this->alternateMessageContrast_, u"%"_s));
    messageForm->addRow(
        u"Highlighted messages:"_s,
        makeOpacityAdjustmentSlider(&this->highlightOpacityAdjustment_));
    this->highlightOpacityAdjustment_->setToolTip(
        u"Makes highlight colors fainter (left) or stronger (right)."_s);
    layout->addWidget(messages);

    auto *frame = new QGroupBox(u"Message area"_s);
    auto *frameForm = new QFormLayout(frame);
    frameForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->roundChat_ = new QCheckBox(u"Round the message area"_s);
    frameForm->addRow(this->roundChat_);
    frameForm->addRow(u"Corner radius:"_s,
                      makeSlider(0, 16, &this->chatCornerRadius_));
    this->chatBorder_ = new QCheckBox(u"Draw an inner border"_s);
    frameForm->addRow(this->chatBorder_);
    this->chatBorderColor_ = this->makeColorControl(
        u"Color of the border."_s, &ThemeCustomizationProfile::chatBorderColor);
    frameForm->addRow(u"Border color:"_s, this->chatBorderColor_);
    frameForm->addRow(u"Border width:"_s,
                      makeSlider(1, 4, &this->chatBorderWidth_));
    frameForm->addRow(u"Border strength:"_s,
                      makeSlider(0, 100, &this->chatBorderOpacity_, u"%"_s));
    layout->addWidget(frame);

    auto *shadow = new QGroupBox(u"Readability shadow"_s);
    auto *shadowForm = new QFormLayout(shadow);
    shadowForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->messageShadow_ = new QCheckBox(u"Add a shadow behind chat content"_s);
    this->messageShadow_->setToolTip(u"Animated emotes don't get a shadow."_s);
    shadowForm->addRow(this->messageShadow_);
    this->messageShadowEmotes_ = new QCheckBox(u"Include emotes and emoji"_s);
    shadowForm->addRow(this->messageShadowEmotes_);
    this->messageShadowColor_ =
        this->makeColorControl(u"Color of the shadow."_s,
                               &ThemeCustomizationProfile::messageShadowColor);
    shadowForm->addRow(u"Color:"_s, this->messageShadowColor_);
    shadowForm->addRow(
        u"Strength:"_s,
        makeSlider(0, 100, &this->messageShadowOpacity_, u"%"_s));
    this->messageShadowPosition_ = new ShadowPositionWidget;
    this->messageShadowPosition_->positionChanged = [this](QPoint offset) {
        if (this->loadingControls_)
        {
            return;
        }
        this->profile_.messageShadowOffsetX = offset.x();
        this->profile_.messageShadowOffsetY = offset.y();
        this->updatePreview();
    };
    auto *positionRow = new QWidget;
    auto *positionLayout = new QHBoxLayout(positionRow);
    positionLayout->setContentsMargins(0, 0, 0, 0);
    positionLayout->setSpacing(8);
    positionLayout->addWidget(this->messageShadowPosition_);
    this->messageShadowCenter_ = new QPushButton(u"Center shadow"_s);
    positionLayout->addWidget(this->messageShadowCenter_);
    positionLayout->addStretch(1);
    shadowForm->addRow(u"Position:"_s, positionRow);
    QObject::connect(this->messageShadowCenter_, &QPushButton::clicked, this,
                     [this] {
                         this->profile_.messageShadowOffsetX = 0;
                         this->profile_.messageShadowOffsetY = 0;
                         this->loadProfileIntoControls();
                     });
    shadowForm->addRow(u"Blur:"_s, makeSlider(0, 8, &this->messageShadowBlur_));
    layout->addWidget(shadow);
    layout->addStretch(1);

    const auto update = [this] {
        if (!this->loadingControls_)
        {
            this->readControlsIntoProfile();
        }
    };
    for (auto *slider :
         {this->alternateMessageOpacity_, this->alternateMessageContrast_,
          this->highlightOpacityAdjustment_, this->chatCornerRadius_,
          this->chatBorderWidth_, this->chatBorderOpacity_,
          this->messageShadowOpacity_, this->messageShadowBlur_})
    {
        QObject::connect(slider, &QSlider::valueChanged, this, update);
    }
    for (auto *box :
         {this->alternateMessageRows_, this->roundChat_, this->chatBorder_,
          this->messageShadow_, this->messageShadowEmotes_})
    {
        QObject::connect(box, &QCheckBox::toggled, this, update);
    }
    return page;
}

QWidget *CustomizationPage::buildInterfaceTab()
{
    auto *page = new QWidget;
    this->interfaceEditor_ = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *style = new QGroupBox(u"Style"_s);
    auto *styleForm = new QFormLayout(style);
    this->foundation_ = new QComboBox;
    this->foundation_->addItems({u"Classic"_s, u"Polished"_s});
    this->foundation_->setToolTip(
        u"Classic has lines around splits and the input and marks the "
        u"focused split. Polished leaves them out."_s);
    styleForm->addRow(u"Look:"_s, this->foundation_);
    layout->addWidget(style);

    auto *tabs = new QGroupBox(u"Tabs"_s);
    auto *tabForm = new QFormLayout(tabs);
    tabForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->tabShape_ = new QComboBox;
    this->tabShape_->addItems({u"Connected"_s, u"Individual"_s});
    this->tabShape_->setToolTip(
        u"Connected rounds the ends of the row of tabs, Individual rounds "
        u"every tab."_s);
    tabForm->addRow(u"Shape:"_s, this->tabShape_);
    tabForm->addRow(u"Corner radius:"_s,
                    makeSlider(0, 10, &this->tabCornerRadius_));
    tabForm->addRow(u"Spacing:"_s, makeSlider(0, 4, &this->tabSpacing_));
    layout->addWidget(tabs);
    layout->addStretch(1);

    const auto update = [this] {
        if (!this->loadingControls_)
        {
            this->readControlsIntoProfile();
        }
    };
    for (auto *slider : {this->tabCornerRadius_, this->tabSpacing_})
    {
        QObject::connect(slider, &QSlider::valueChanged, this, update);
    }
    for (auto *box : {this->foundation_, this->tabShape_})
    {
        QObject::connect(box, &QComboBox::currentIndexChanged, this, update);
    }
    return page;
}

QWidget *CustomizationPage::buildTextTab()
{
    auto *page = new QWidget;
    this->textEditor_ = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto *description = new QLabel(
        u"A theme can bring its own fonts. Without them, the fonts from the "
        u"General settings are used."_s);
    description->setWordWrap(true);
    layout->addWidget(description);

    this->typographyPreview_ = new TypographyPreviewWidget;
    layout->addWidget(this->typographyPreview_);

    this->useThemeFonts_ = new QCheckBox(u"Use theme fonts"_s);
    this->useThemeFonts_->setToolTip(
        u"Font families and weights of this theme replace the ones from "
        u"the General settings."_s);
    this->useThemeFontSizes_ = new QCheckBox(u"Use theme font sizes"_s);
    layout->addWidget(this->useThemeFonts_);
    layout->addWidget(this->useThemeFontSizes_);

    const auto families = QFontDatabase::families();
    const auto makeFontBox = [&families] {
        auto *box = new QComboBox;
        box->setEditable(false);
        box->setMaxVisibleItems(18);
        for (const auto &family : families)
        {
            box->addItem(family, family);
        }
        return box;
    };

    auto *chat = new QGroupBox(u"Chat"_s);
    auto *chatForm = new QFormLayout(chat);
    chatForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->chatFont_ = makeFontBox();
    chatForm->addRow(u"Font:"_s, this->chatFont_);
    chatForm->addRow(u"Size:"_s, makeSlider(6, 40, &this->chatFontSize_));
    chatForm->addRow(u"Weight:"_s,
                     makeSlider(100, 900, &this->chatFontWeight_));
    layout->addWidget(chat);

    auto *usernames = new QGroupBox(u"Usernames"_s);
    auto *usernameForm = new QFormLayout(usernames);
    usernameForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->usernameFont_ = makeFontBox();
    usernames->setToolTip(u"Also used for other bold text in chat."_s);
    usernameForm->addRow(u"Font:"_s, this->usernameFont_);
    usernameForm->addRow(u"Weight:"_s,
                         makeSlider(100, 900, &this->usernameFontWeight_));
    layout->addWidget(usernames);

    auto *interfaceGroup = new QGroupBox(u"Interface"_s);
    auto *interfaceForm = new QFormLayout(interfaceGroup);
    interfaceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->interfaceFont_ = makeFontBox();
    interfaceGroup->setToolTip(
        u"Tabs, the channel bar and the input. Some parts only pick it up "
        u"after a restart."_s);
    interfaceForm->addRow(u"Font:"_s, this->interfaceFont_);
    interfaceForm->addRow(u"Size:"_s,
                          makeSlider(6, 30, &this->interfaceFontSize_));
    layout->addWidget(interfaceGroup);
    layout->addStretch(1);

    for (auto *slider : {this->chatFontWeight_, this->usernameFontWeight_})
    {
        slider->setSingleStep(100);
        slider->setPageStep(100);
        slider->setTickInterval(100);
    }

    const auto update = [this] {
        if (!this->loadingControls_)
        {
            this->readControlsIntoProfile();
        }
    };
    for (auto *slider : {this->chatFontSize_, this->chatFontWeight_,
                         this->usernameFontWeight_, this->interfaceFontSize_})
    {
        QObject::connect(slider, &QSlider::valueChanged, this, update);
    }
    for (auto *box :
         {this->chatFont_, this->usernameFont_, this->interfaceFont_})
    {
        QObject::connect(box, &QComboBox::currentIndexChanged, this, update);
    }
    for (auto *box : {this->useThemeFonts_, this->useThemeFontSizes_})
    {
        QObject::connect(box, &QCheckBox::toggled, this, update);
    }
    return page;
}

QWidget *CustomizationPage::buildBackgroundTab()
{
    auto *page = new QWidget;
    this->backgroundEditor_ = page;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto *description =
        new QLabel(u"Show an image behind the chat messages."_s);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *wallpaper = new QGroupBox(u"Wallpaper"_s);
    auto *form = new QFormLayout(wallpaper);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *pathRow = new QWidget;
    auto *pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->setSpacing(5);
    this->wallpaperPath_ = new QLineEdit;
    this->wallpaperPath_->setReadOnly(true);
    this->wallpaperPath_->setPlaceholderText(u"No image"_s);
    auto *choose = new QPushButton(u"Choose image…"_s);
    auto *clear = new QPushButton(u"Remove"_s);
    pathLayout->addWidget(this->wallpaperPath_, 1);
    pathLayout->addWidget(choose);
    pathLayout->addWidget(clear);
    form->addRow(u"Background:"_s, pathRow);
    this->wallpaperMode_ = new QComboBox;
    this->wallpaperMode_->addItems({u"Fill chat"_s, u"Fit whole background"_s,
                                    u"Original size"_s, u"Tile"_s,
                                    u"Stretch"_s});
    form->addRow(u"Placement:"_s, this->wallpaperMode_);
    form->addRow(u"Zoom:"_s,
                 makeSlider(100, 300, &this->wallpaperZoom_, u"%"_s));
    this->wallpaperZoom_->setToolTip(u"Zoom around the focus point."_s);
    this->wallpaperFocus_ = new WallpaperFocusWidget;
    this->wallpaperFocus_->focusChanged = [this](int x, int y) {
        this->profile_.wallpaperFocalX = x;
        this->profile_.wallpaperFocalY = y;
        this->updatePreview();
    };
    form->addRow(u"Focus:"_s, this->wallpaperFocus_);
    auto *focusHelp = new QWidget;
    auto *focusHelpLayout = new QHBoxLayout(focusHelp);
    focusHelpLayout->setContentsMargins(0, 0, 0, 0);
    focusHelpLayout->setSpacing(6);
    auto *focusHint = new QLabel(u"Keep this part in view."_s);
    focusHint->setWordWrap(true);
    focusHelpLayout->addWidget(focusHint, 1);
    this->wallpaperCenterFocus_ = new QPushButton(u"Center"_s);
    this->wallpaperCenterFocus_->setToolTip(u"Center the focus point."_s);
    focusHelpLayout->addWidget(this->wallpaperCenterFocus_);
    form->addRow(QString(), focusHelp);
    QObject::connect(this->wallpaperCenterFocus_, &QPushButton::clicked, this,
                     [this] {
                         this->profile_.wallpaperFocalX = 50;
                         this->profile_.wallpaperFocalY = 50;
                         this->updatePreview();
                     });
    form->addRow(u"Wallpaper opacity:"_s,
                 makeSlider(0, 100, &this->wallpaperOpacity_, u"%"_s));
    this->wallpaperOverlayColor_ = this->makeColorControl(
        u"Blended over the wallpaper so text stays readable."_s,
        &ThemeCustomizationProfile::wallpaperOverlayColor);
    form->addRow(u"Overlay color:"_s, this->wallpaperOverlayColor_);
    form->addRow(u"Overlay opacity:"_s,
                 makeSlider(0, 100, &this->wallpaperOverlayOpacity_, u"%"_s));
    this->wallpaperOverlayOpacity_->setToolTip(
        u"Blends the overlay color over the wallpaper."_s);
    form->addRow(u"Blur:"_s, makeSlider(0, 30, &this->wallpaperBlur_));
    layout->addWidget(wallpaper);

    auto *palette = new QGroupBox(u"Colors"_s);
    auto *paletteLayout = new QHBoxLayout(palette);
    auto *paletteHint =
        new QLabel(u"Pick the theme's colors from the wallpaper."_s);
    paletteHint->setWordWrap(true);
    paletteLayout->addWidget(paletteHint, 1);
    this->wallpaperColors_ = new QPushButton(u"Use colors from wallpaper"_s);
    paletteLayout->addWidget(this->wallpaperColors_);
    layout->addWidget(palette);
    layout->addStretch(1);

    const auto update = [this] {
        if (!this->loadingControls_)
        {
            this->readControlsIntoProfile();
        }
    };
    for (auto *slider : {this->wallpaperZoom_, this->wallpaperOpacity_,
                         this->wallpaperOverlayOpacity_, this->wallpaperBlur_})
    {
        QObject::connect(slider, &QSlider::valueChanged, this, update);
    }
    QObject::connect(this->wallpaperMode_, &QComboBox::currentIndexChanged,
                     this, update);
    QObject::connect(choose, &QPushButton::clicked, this, [this] {
        this->chooseWallpaper();
    });
    QObject::connect(clear, &QPushButton::clicked, this, [this] {
        this->clearWallpaper();
    });
    QObject::connect(this->wallpaperColors_, &QPushButton::clicked, this,
                     [this] {
                         this->suggestWallpaperPalette();
                     });
    return page;
}

void CustomizationPage::chooseWallpaper()
{
    const auto generation = this->profileGeneration_;
    const auto path = QFileDialog::getOpenFileName(this, u"Choose an image"_s,
                                                   {}, wallpaperFileFilter());
    if (path.isEmpty() || generation != this->profileGeneration_)
    {
        return;
    }
    if (QFileInfo(path).size() > MAX_WALLPAPER_FILE_SIZE ||
        loadThemeWallpaper(path, 256).isNull())
    {
        this->setStatus(
            u"That image couldn't be read. Use a PNG, JPEG, WebP, GIF or "
            u"BMP of up to 50 MB."_s,
            true);
        return;
    }
    this->readControlsIntoProfile();
    this->profile_.wallpaperSource = path;
    this->profile_.wallpaperId.clear();
    this->loadProfileIntoControls();
    this->setStatus({});
}

void CustomizationPage::clearWallpaper()
{
    this->readControlsIntoProfile();
    this->profile_.wallpaperSource.clear();
    this->profile_.wallpaperId.clear();
    this->loadProfileIntoControls();
    this->setStatus({});
}

void CustomizationPage::suggestWallpaperPalette()
{
    auto image = this->preview_->wallpaperFrame();
    if (image.isNull())
    {
        this->setStatus(u"Choose a wallpaper first."_s, true);
        return;
    }
    this->readControlsIntoProfile();
    image = image.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    qint64 red = 0;
    qint64 green = 0;
    qint64 blue = 0;
    qint64 count = 0;
    QColor accent = this->profile_.accent;
    qreal bestAccent = -1;
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            const QColor color = image.pixelColor(x, y);
            red += color.red();
            green += color.green();
            blue += color.blue();
            ++count;
            // colorful and neither too dark nor too bright
            const qreal score = color.hsvSaturationF() *
                                (0.35 + color.valueF()) *
                                (1.0 - qAbs(color.valueF() - 0.62));
            if (score > bestAccent)
            {
                bestAccent = score;
                accent = color;
            }
        }
    }
    if (count == 0)
    {
        return;
    }
    QColor average(static_cast<int>(red / count),
                   static_cast<int>(green / count),
                   static_cast<int>(blue / count));
    if (average.lightnessF() > 0.35)
    {
        average = average.darker(250);
    }
    this->profile_.background = average.darker(145);
    this->profile_.chatBackground = average.darker(125);
    this->profile_.surface = average.lighter(135);
    this->profile_.raisedSurface = average.lighter(165);
    this->profile_.accent =
        accent.lightnessF() < 0.48 ? accent.lighter(150) : accent;
    this->profile_.text = Qt::white;
    this->profile_.chatText = this->profile_.text;
    this->profile_.separateChatText = false;
    this->profile_.mutedText = QColor(u"#b9b9c2"_s);
    this->profile_.systemText = this->profile_.mutedText;
    this->profile_.timestampText = this->profile_.mutedText;
    this->profile_.chatBorderColor = this->profile_.mutedText;
    this->profile_.wallpaperOverlayColor = average.darker(160);
    this->loadProfileIntoControls();
    this->setStatus(
        u"Wallpaper colors applied. You can fine tune them in the Colors "
        u"tab."_s);
}

QString CustomizationPage::managedWallpaperSource(const QString &source,
                                                  QString *error) const
{
    if (source.isEmpty() || source.startsWith(u":/"_s))
    {
        return source;
    }

    const QFileInfo sourceInfo(source);
    if (!sourceInfo.isFile() || sourceInfo.size() > MAX_WALLPAPER_FILE_SIZE)
    {
        *error = u"The background image is missing or larger than 50 MB."_s;
        return {};
    }

    const QDir themes(getTheme()->themesDirectory());
    const auto directoryPath = themes.filePath(u"Wallpapers"_s);
    if (!QDir().mkpath(directoryPath))
    {
        *error = u"The folder for background images couldn't be created."_s;
        return {};
    }
    const auto absoluteSource = QDir::fromNativeSeparators(
        QDir::cleanPath(sourceInfo.absoluteFilePath()));
    if (loadThemeWallpaper(absoluteSource, 256).isNull())
    {
        *error = u"That background image couldn't be read."_s;
        return {};
    }
    // Already one of our copies.
    if (normalizedLocalPath(absoluteSource)
            .startsWith(normalizedLocalPath(directoryPath) + u'/'))
    {
        return absoluteSource;
    }

    const auto fingerprint =
        u"%1\n%2\n%3"_s.arg(absoluteSource)
            .arg(sourceInfo.size())
            .arg(sourceInfo.lastModified().toMSecsSinceEpoch())
            .toUtf8();
    const auto digest =
        QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha256)
            .toHex()
            .left(16);
    auto suffix = sourceInfo.suffix().toLower();
    if (suffix.isEmpty())
    {
        suffix = u"png"_s;
    }
    const auto destination = QDir(directoryPath)
                                 .filePath(u"wallpaper-%1.%2"_s.arg(
                                     QString::fromLatin1(digest), suffix));
    if (!QFileInfo::exists(destination) &&
        !QFile::copy(absoluteSource, destination))
    {
        *error = u"A copy of the background image couldn't be saved."_s;
        return {};
    }
    return destination;
}

void CustomizationPage::cleanupManagedWallpapers() const
{
    const QDir themes(getTheme()->themesDirectory());
    const QDir wallpapers(themes.filePath(u"Wallpapers"_s));
    if (!wallpapers.exists())
    {
        return;
    }

    // Every background a theme file still names. If one theme can't be read
    // we don't know what it uses, so nothing is removed.
    QSet<QString> referenced;
    const auto themeFiles = themes.entryInfoList(
        {u"*.json"_s}, QDir::Files | QDir::Hidden, QDir::Name);
    for (const auto &themeInfo : themeFiles)
    {
        QFile file(themeInfo.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
        {
            return;
        }
        QJsonParseError parseError{};
        const auto document =
            QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            !document.isObject())
        {
            return;
        }
        const auto source = document.object()
                                .value(u"metadata"_s)
                                .toObject()
                                .value(u"moltorino"_s)
                                .toObject()
                                .value(u"wallpaper"_s)
                                .toObject()
                                .value(u"source"_s)
                                .toString();
        if (!source.isEmpty() && !source.startsWith(u":/"_s))
        {
            referenced.insert(normalizedLocalPath(source));
        }
    }
    if (!this->profile_.wallpaperSource.isEmpty() &&
        !this->profile_.wallpaperSource.startsWith(u":/"_s))
    {
        referenced.insert(normalizedLocalPath(this->profile_.wallpaperSource));
    }

    static const QRegularExpression managedName(
        uR"(^wallpaper-[0-9a-f]{16}\.[a-z0-9]+$)"_s,
        QRegularExpression::CaseInsensitiveOption);
    const auto files =
        wallpapers.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const auto &file : files)
    {
        if (managedName.match(file.fileName()).hasMatch() &&
            !referenced.contains(normalizedLocalPath(file.absoluteFilePath())))
        {
            QFile::remove(file.absoluteFilePath());
        }
    }
}

QWidget *CustomizationPage::buildShareTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto *description = new QLabel(
        u"Save the selected theme as a file to share it, or import a theme "
        u"file. Theme files from Moltorino and Bluzyrino can be imported "
        u"too; their video backgrounds are kept in the theme but aren't "
        u"used here. A background image stored on your computer "
        u"isn't part of a saved theme file."_s);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *files = new QGroupBox(u"Theme files"_s);
    auto *fileLayout = new QHBoxLayout(files);
    this->exportFileButton_ = new QPushButton(u"Save theme file…"_s);
    auto *importFileButton = new QPushButton(u"Import theme file…"_s);
    fileLayout->addWidget(this->exportFileButton_);
    fileLayout->addWidget(importFileButton);
    fileLayout->addStretch(1);
    layout->addWidget(files);
    layout->addStretch(1);

    QObject::connect(this->exportFileButton_, &QPushButton::clicked, this,
                     [this] {
                         this->exportThemeFile();
                     });
    QObject::connect(importFileButton, &QPushButton::clicked, this, [this] {
        this->importThemeFile();
    });
    return page;
}

void CustomizationPage::onShow()
{
    if (this->selectedCustom_ && this->profile_ != this->savedProfile_)
    {
        return;
    }
    this->reloadProfiles(this->selectedKey_.isEmpty()
                             ? getTheme()->themeName.getValue()
                             : this->selectedKey_);
}

bool CustomizationPage::filterElements(const QString &query)
{
    return query.isEmpty() ||
           u"customization themes colors palette chat interface text tabs "
           u"contrast timestamps system messages accent background surface "
           u"rows highlights corners rounding border shadow shape spacing "
           u"wallpaper image blur overlay fonts typography username weight "
           u"import export moltorino bluzyrino"_s.contains(
               query, Qt::CaseInsensitive) ||
           SettingsPage::filterElements(query);
}

void CustomizationPage::reloadProfiles(const QString &preferredKey)
{
    getTheme()->reloadAvailableThemes();
    {
        const QSignalBlocker blocker(this->profileBox_);
        this->profileBox_->clear();
        int selected = 0;
        int index = 0;
        for (const auto &descriptor : getTheme()->availableThemeDescriptors())
        {
            this->profileBox_->addItem(descriptor.name, descriptor.key);
            if (descriptor.key == preferredKey)
            {
                selected = index;
            }
            ++index;
        }
        this->profileBox_->setCurrentIndex(selected);
    }
    this->loadSelectedProfile();
}

bool CustomizationPage::confirmProfileChange()
{
    if (!this->selectedCustom_ || this->profile_ == this->savedProfile_)
    {
        return true;
    }

    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(u"Unsaved theme"_s);
    prompt.setText(u"Save your changes to %1?"_s.arg(this->profile_.name));
    prompt.setStandardButtons(QMessageBox::Save | QMessageBox::Discard |
                              QMessageBox::Cancel);
    prompt.setDefaultButton(QMessageBox::Save);
    const auto generation = this->profileGeneration_;
    const auto choice = prompt.exec();
    if (choice == QMessageBox::Cancel || generation != this->profileGeneration_)
    {
        return false;
    }
    if (choice == QMessageBox::Save)
    {
        return this->saveCustomProfile(false);
    }
    return true;
}

void CustomizationPage::loadSelectedProfile()
{
    this->setStatus({});
    const auto key = this->profileBox_->currentData().toString();
    for (const auto &descriptor : getTheme()->availableThemeDescriptors())
    {
        if (descriptor.key != key)
        {
            continue;
        }
        const auto json = getTheme()->themeJson(key);
        if (!json)
        {
            return;
        }
        ++this->profileGeneration_;
        this->selectedKey_ = key;
        this->selectedPath_ = descriptor.path;
        this->selectedCustom_ = descriptor.custom;
        this->editedColorRoles_.clear();
        this->profile_ = profileFromTheme(descriptor, *json);
        this->savedProfile_ = this->profile_;
        this->loadProfileIntoControls();
        this->setStatus(this->selectedCustom_
                            ? QString()
                            : u"Choose Customize to edit a copy."_s);
        return;
    }
}

void CustomizationPage::loadProfileIntoControls()
{
    if (!this->profile_.separateChatText || !this->profile_.chatText.isValid())
    {
        this->profile_.chatText = this->profile_.text;
    }
    if (!this->profile_.systemText.isValid())
    {
        this->profile_.systemText = this->profile_.mutedText;
    }
    if (!this->profile_.timestampText.isValid())
    {
        this->profile_.timestampText = this->profile_.systemText;
    }
    this->loadingControls_ = true;
    this->chatTextFollowsText_->setChecked(!this->profile_.separateChatText);
    for (auto &control : this->colorControls_)
    {
        const auto color = this->profile_.*control.member;
        control.input->setText(color.name(
            color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb));
        control.button->setStyleSheet(
            u"background:%1; border:1px solid %2;"_s.arg(
                color.name(QColor::HexRgb),
                this->profile_.mutedText.name(QColor::HexRgb)));
    }
    this->panelContrast_->setValue(this->profile_.panelContrast);
    this->accentStrength_->setCurrentIndex(
        static_cast<int>(this->profile_.accentStrength));
    this->inactiveTabContrast_->setValue(this->profile_.inactiveTabContrast);
    this->channelBarContrast_->setValue(this->profile_.channelBarContrast);
    {
        // What is shown while the theme doesn't set a font itself.
        const auto chatFamily = getSettings()->chatFontFamily.getValue();
        const auto selectFont = [](QComboBox *box, const QString &family,
                                   const QString &fallback) {
            const auto &wanted = family.isEmpty() ? fallback : family;
            auto index = box->findData(wanted);
            if (index < 0)
            {
                // not installed here; keep it so saving doesn't lose it
                box->addItem(wanted, wanted);
                index = box->count() - 1;
            }
            box->setCurrentIndex(index);
        };
        this->useThemeFonts_->setChecked(this->profile_.useThemeFonts);
        this->useThemeFontSizes_->setChecked(this->profile_.useThemeFontSizes);
        selectFont(this->chatFont_, this->profile_.chatFontFamily, chatFamily);
        selectFont(this->usernameFont_, this->profile_.usernameFontFamily,
                   this->profile_.chatFontFamily.isEmpty()
                       ? chatFamily
                       : this->profile_.chatFontFamily);
        selectFont(this->interfaceFont_, this->profile_.interfaceFontFamily,
                   QStringLiteral(DEFAULT_FONT_FAMILY));
        this->chatFontSize_->setValue(
            this->profile_.chatFontSize > 0
                ? this->profile_.chatFontSize
                : getSettings()->chatFontSize.getValue());
        this->chatFontWeight_->setValue(
            this->profile_.chatFontWeight > 0
                ? this->profile_.chatFontWeight
                : getSettings()->chatFontWeight.getValue());
        this->usernameFontWeight_->setValue(
            this->profile_.usernameFontWeight > 0
                ? this->profile_.usernameFontWeight
                : 700);
        this->interfaceFontSize_->setValue(
            this->profile_.interfaceFontSize > 0
                ? this->profile_.interfaceFontSize
                : 9);
    }
    this->wallpaperPath_->setText(
        QFileInfo(this->profile_.wallpaperSource).fileName());
    this->wallpaperPath_->setToolTip(this->profile_.wallpaperSource);
    this->wallpaperMode_->setCurrentIndex(
        static_cast<int>(this->profile_.wallpaperMode));
    this->wallpaperZoom_->setValue(this->profile_.wallpaperZoom);
    this->wallpaperOpacity_->setValue(this->profile_.wallpaperOpacity);
    this->wallpaperOverlayOpacity_->setValue(
        this->profile_.wallpaperOverlayOpacity);
    this->wallpaperBlur_->setValue(this->profile_.wallpaperBlur);
    this->alternateMessageRows_->setChecked(
        this->profile_.alternateMessageRows);
    this->alternateMessageOpacity_->setValue(
        this->profile_.alternateMessageOpacity);
    this->alternateMessageContrast_->setValue(
        this->profile_.alternateMessageContrast);
    this->highlightOpacityAdjustment_->setValue(
        this->profile_.highlightOpacityAdjustment);
    this->roundChat_->setChecked(this->profile_.roundChat);
    this->chatCornerRadius_->setValue(this->profile_.chatCornerRadius);
    this->chatBorder_->setChecked(this->profile_.chatBorder);
    this->chatBorderWidth_->setValue(this->profile_.chatBorderWidth);
    this->chatBorderOpacity_->setValue(this->profile_.chatBorderOpacity);
    this->messageShadow_->setChecked(this->profile_.messageShadow);
    this->messageShadowEmotes_->setChecked(this->profile_.messageShadowEmotes);
    this->messageShadowOpacity_->setValue(this->profile_.messageShadowOpacity);
    this->messageShadowPosition_->setOffset(
        this->profile_.messageShadowOffset());
    this->messageShadowPosition_->setShadowColor(
        this->profile_.messageShadowColor);
    this->messageShadowBlur_->setValue(this->profile_.messageShadowBlur);
    this->foundation_->setCurrentIndex(
        this->profile_.foundation == ThemeFoundation::MoltorinoPolished ? 1
                                                                        : 0);
    this->tabShape_->setCurrentIndex(
        this->profile_.tabShape == ThemeTabShape::Individual ? 1 : 0);
    this->tabCornerRadius_->setValue(this->profile_.tabCornerRadius);
    this->tabSpacing_->setValue(this->profile_.tabSpacing);
    this->loadingControls_ = false;
    this->updatePreview();
}

void CustomizationPage::readControlsIntoProfile()
{
    if (this->loadingControls_)
    {
        return;
    }
    for (auto &control : this->colorControls_)
    {
        const QColor color(control.input->text().trimmed());
        if (color.isValid())
        {
            this->profile_.*control.member = color;
        }
    }
    this->profile_.separateChatText = !this->chatTextFollowsText_->isChecked();
    if (!this->profile_.separateChatText)
    {
        this->profile_.chatText = this->profile_.text;
    }
    this->profile_.panelContrast = this->panelContrast_->value();
    this->profile_.accentStrength = static_cast<ThemeAccentStrength>(
        std::clamp(this->accentStrength_->currentIndex(), 0, 2));
    this->profile_.inactiveTabContrast = this->inactiveTabContrast_->value();
    this->profile_.channelBarContrast = this->channelBarContrast_->value();
    this->profile_.useThemeFonts = this->useThemeFonts_->isChecked();
    this->profile_.useThemeFontSizes = this->useThemeFontSizes_->isChecked();
    // Only fonts the theme really uses are stored, so turning a box off
    // leaves no half-set fonts behind.
    if (this->profile_.useThemeFonts)
    {
        this->profile_.chatFontFamily =
            this->chatFont_->currentData().toString();
        this->profile_.usernameFontFamily =
            this->usernameFont_->currentData().toString();
        this->profile_.interfaceFontFamily =
            this->interfaceFont_->currentData().toString();
        this->profile_.chatFontWeight = this->chatFontWeight_->value();
        this->profile_.usernameFontWeight = this->usernameFontWeight_->value();
    }
    if (this->profile_.useThemeFontSizes)
    {
        this->profile_.chatFontSize = this->chatFontSize_->value();
        this->profile_.usernameFontSize = this->chatFontSize_->value();
        this->profile_.interfaceFontSize = this->interfaceFontSize_->value();
    }
    this->profile_.clearDisabledFontOverrides();
    this->profile_.wallpaperMode = static_cast<ThemeWallpaperMode>(
        std::clamp(this->wallpaperMode_->currentIndex(), 0, 4));
    this->profile_.wallpaperZoom = this->wallpaperZoom_->value();
    this->profile_.wallpaperOpacity = this->wallpaperOpacity_->value();
    this->profile_.wallpaperOverlayOpacity =
        this->wallpaperOverlayOpacity_->value();
    this->profile_.wallpaperBlur = this->wallpaperBlur_->value();
    // Rows are the theme's choice once it is edited here.
    this->profile_.useThemeMessageRows = true;
    this->profile_.alternateMessageRows =
        this->alternateMessageRows_->isChecked();
    this->profile_.alternateMessageOpacity =
        this->alternateMessageOpacity_->value();
    this->profile_.alternateMessageContrast =
        this->alternateMessageContrast_->value();
    this->profile_.highlightOpacityAdjustment =
        this->highlightOpacityAdjustment_->value();
    this->profile_.roundChat = this->roundChat_->isChecked();
    this->profile_.chatCornerRadius = this->chatCornerRadius_->value();
    this->profile_.chatBorder = this->chatBorder_->isChecked();
    this->profile_.chatBorderWidth = this->chatBorderWidth_->value();
    this->profile_.chatBorderOpacity = this->chatBorderOpacity_->value();
    this->profile_.messageShadow = this->messageShadow_->isChecked();
    this->profile_.messageShadowEmotes =
        this->messageShadowEmotes_->isChecked();
    this->profile_.messageShadowOpacity = this->messageShadowOpacity_->value();
    this->profile_.messageShadowBlur = this->messageShadowBlur_->value();
    this->profile_.foundation = this->foundation_->currentIndex() == 1
                                    ? ThemeFoundation::MoltorinoPolished
                                    : ThemeFoundation::ChatterinoClassic;
    this->profile_.tabShape = this->tabShape_->currentIndex() == 1
                                  ? ThemeTabShape::Individual
                                  : ThemeTabShape::Connected;
    this->profile_.tabCornerRadius = this->tabCornerRadius_->value();
    this->profile_.tabSpacing = this->tabSpacing_->value();
    this->updatePreview();
}

void CustomizationPage::updatePreview()
{
    this->preview_->setProfile(this->profile_);
    this->wallpaperFocus_->setProfile(this->profile_);
    this->typographyPreview_->setProfile(this->profile_);
    this->updateReadability();
    this->updateControlState();
}

void CustomizationPage::updateReadability()
{
    const double chat = colorContrastRatio(this->profile_.effectiveChatText(),
                                           this->profile_.chatBackground);
    const double surface =
        colorContrastRatio(this->profile_.text, this->profile_.surface);
    const double raised =
        colorContrastRatio(this->profile_.text, this->profile_.raisedSurface);
    const double background =
        colorContrastRatio(this->profile_.text, this->profile_.background);
    const double interfaceContrast = std::min({background, surface, raised});
    const double accentContrast = std::min(
        {colorContrastRatio(this->profile_.accent, this->profile_.background),
         colorContrastRatio(this->profile_.accent, this->profile_.surface),
         colorContrastRatio(this->profile_.accent,
                            this->profile_.raisedSurface)});
    if (chat < 3.0 || interfaceContrast < 3.0)
    {
        this->readability_->setText(
            chat < 3.0 && interfaceContrast < 3.0
                ? u"Low text contrast in chat and the interface."_s
            : chat < 3.0 ? u"Low chat text contrast."_s
                         : u"Low interface text contrast."_s);
        this->readability_->setStyleSheet(u"color:#ff9b63;"_s);
    }
    else if (accentContrast < 3.0)
    {
        this->readability_->setText(u"Low accent contrast."_s);
        this->readability_->setStyleSheet(u"color:#ff9b63;"_s);
    }
    else if (chat < 4.5 || interfaceContrast < 4.5)
    {
        this->readability_->setText(
            chat < 4.5 && interfaceContrast < 4.5
                ? u"Text contrast could be higher."_s
            : chat < 4.5 ? u"Chat text could use more contrast."_s
                         : u"Interface text could use more contrast."_s);
        this->readability_->setStyleSheet(u"color:#e1be64;"_s);
    }
    else
    {
        this->readability_->setText(u"Text contrast looks good."_s);
        this->readability_->setStyleSheet(u"color:#73ca92;"_s);
    }
}

void CustomizationPage::updateControlState()
{
    this->profileKind_->setText(this->selectedCustom_ ? u"Custom"_s
                                                      : u"Included"_s);
    this->duplicateButton_->setText(this->selectedCustom_ ? u"Duplicate"_s
                                                          : u"Customize"_s);
    this->duplicateButton_->setToolTip(this->selectedCustom_
                                           ? u"Make a copy of this theme."_s
                                           : u"Make an editable copy."_s);
    this->renameButton_->setEnabled(this->selectedCustom_);
    this->deleteButton_->setEnabled(this->selectedCustom_);
    const bool dirty =
        this->selectedCustom_ && this->profile_ != this->savedProfile_;
    this->saveButton_->setEnabled(dirty);
    this->resetButton_->setEnabled(dirty);
    this->useButton_->setEnabled(this->selectedKey_ !=
                                 getTheme()->themeName.getValue());
    const bool editable = this->selectedCustom_;
    this->colorsEditor_->setEnabled(editable);
    this->chatEditor_->setEnabled(editable);
    this->interfaceEditor_->setEnabled(editable);
    this->backgroundEditor_->setEnabled(editable);
    this->textEditor_->setEnabled(editable);
    for (QWidget *widget : std::initializer_list<QWidget *>{
             this->chatFont_, this->usernameFont_, this->interfaceFont_,
             this->chatFontWeight_, this->usernameFontWeight_})
    {
        widget->setEnabled(editable && this->profile_.useThemeFonts);
    }
    for (auto *slider : {this->chatFontSize_, this->interfaceFontSize_})
    {
        slider->setEnabled(editable && this->profile_.useThemeFontSizes);
    }
    const bool wallpaper =
        editable && !this->profile_.wallpaperSource.isEmpty();
    for (QWidget *widget : std::initializer_list<QWidget *>{
             this->wallpaperMode_, this->wallpaperZoom_,
             this->wallpaperOpacity_, this->wallpaperOverlayColor_,
             this->wallpaperOverlayOpacity_, this->wallpaperBlur_,
             this->wallpaperColors_})
    {
        widget->setEnabled(wallpaper);
    }
    // The focus only matters while part of the image is cut off.
    const bool focusEditable =
        wallpaper && this->profile_.wallpaperMode != ThemeWallpaperMode::Tile &&
        (this->profile_.wallpaperMode == ThemeWallpaperMode::Fill ||
         this->profile_.wallpaperZoom > 100);
    this->wallpaperFocus_->setEnabled(focusEditable);
    this->wallpaperCenterFocus_->setEnabled(focusEditable);
    const bool rows = editable && this->profile_.alternateMessageRows;
    this->alternateMessageOpacity_->setEnabled(rows);
    this->alternateMessageContrast_->setEnabled(rows);
    const bool round = editable && this->profile_.roundChat;
    this->chatCornerRadius_->setEnabled(round);
    this->chatBorder_->setEnabled(round);
    const bool border = round && this->profile_.chatBorder;
    this->chatBorderColor_->setEnabled(border);
    this->chatBorderWidth_->setEnabled(border);
    this->chatBorderOpacity_->setEnabled(border);
    const bool shadow = editable && this->profile_.messageShadow;
    this->messageShadowEmotes_->setEnabled(shadow);
    this->messageShadowColor_->setEnabled(shadow);
    this->messageShadowOpacity_->setEnabled(shadow);
    this->messageShadowPosition_->setEnabled(shadow);
    this->messageShadowCenter_->setEnabled(shadow);
    this->messageShadowBlur_->setEnabled(shadow);
    this->tabSpacing_->setEnabled(editable && this->profile_.tabShape ==
                                                  ThemeTabShape::Individual);
}

void CustomizationPage::applySelectedTheme()
{
    getTheme()->themeName.setValue(this->selectedKey_);
    getApp()->getWindows()->forceLayoutChannelViews();
    this->updateControlState();
    this->setStatus(u"Theme applied."_s);
}

QString CustomizationPage::uniqueThemePath(const QString &name) const
{
    const auto base = safeFileBase(name);
    const QDir directory(getTheme()->themesDirectory());
    for (int suffix = 0; suffix < 1000; ++suffix)
    {
        const auto file = suffix == 0
                              ? base + u".json"_s
                              : u"%1-%2.json"_s.arg(base).arg(suffix + 1);
        const auto path = directory.filePath(file);
        if (!QFileInfo::exists(path))
        {
            return path;
        }
    }
    QString path;
    do
    {
        path = directory.filePath(
            base + u'-' + QUuid::createUuid().toString(QUuid::WithoutBraces) +
            u".json"_s);
    } while (QFileInfo::exists(path));
    return path;
}

void CustomizationPage::duplicateProfile()
{
    if (!this->confirmProfileChange())
    {
        return;
    }
    const auto generation = this->profileGeneration_;
    const auto name = promptForThemeName(
        this,
        this->selectedCustom_ ? u"Duplicate theme"_s : u"Customize theme"_s,
        u"New theme name:"_s, this->profile_.name + u" Copy"_s);
    if (!name || name->trimmed().isEmpty() ||
        generation != this->profileGeneration_)
    {
        return;
    }
    auto copy = this->savedProfile_;
    copy.name = name->trimmed();
    if (!copy.useThemeMessageRows)
    {
        copy.useThemeMessageRows = true;
        copy.alternateMessageRows = getSettings()->alternateMessages.getValue();
    }
    if (!this->selectedCustom_ && !copy.originalColors.isEmpty())
    {
        copy.originalGeneratedColors = {};
        copy.originalGeneratedColors =
            buildCustomizedTheme(copy).value(u"colors"_s).toObject();
    }
    const auto path = this->uniqueThemePath(copy.name);
    if (!QDir().mkpath(getTheme()->themesDirectory()) ||
        !writeThemeFile(path, buildCustomizedTheme(copy)))
    {
        QMessageBox::warning(this, u"Couldn't create theme"_s,
                             u"The new theme file couldn't be saved."_s);
        return;
    }
    this->reloadProfiles(QFileInfo(path).fileName());
    this->setStatus(u"Your editable theme is ready."_s);
}

void CustomizationPage::renameProfile()
{
    if (!this->selectedCustom_)
    {
        return;
    }
    const auto generation = this->profileGeneration_;
    const auto name = promptForThemeName(this, u"Rename theme"_s, u"Name:"_s,
                                         this->profile_.name);
    if (!name || name->trimmed().isEmpty() ||
        generation != this->profileGeneration_)
    {
        return;
    }
    this->profile_.name = name->trimmed();
    if (this->saveCustomProfile(false))
    {
        this->reloadProfiles(this->selectedKey_);
    }
}

void CustomizationPage::deleteProfile()
{
    if (!this->selectedCustom_)
    {
        return;
    }
    const auto generation = this->profileGeneration_;
    if (QMessageBox::question(this, u"Delete theme"_s,
                              u"Delete %1? This cannot be undone."_s.arg(
                                  this->profile_.name)) != QMessageBox::Yes ||
        generation != this->profileGeneration_)
    {
        return;
    }
    const bool active = getTheme()->themeName.getValue() == this->selectedKey_;
    if (!QFile::remove(this->selectedPath_))
    {
        QMessageBox::warning(this, u"Couldn't delete theme"_s,
                             u"That theme file couldn't be removed."_s);
        return;
    }
    // Nothing left to save.
    this->savedProfile_ = this->profile_;
    if (active)
    {
        getTheme()->themeName.setValue(u"Dark"_s);
    }
    this->reloadProfiles(getTheme()->themeName.getValue());
    this->cleanupManagedWallpapers();
    this->setStatus(u"Theme deleted."_s);
}

bool CustomizationPage::saveCustomProfile(bool apply)
{
    if (!this->selectedCustom_)
    {
        return false;
    }
    this->readControlsIntoProfile();
    QString wallpaperError;
    const auto managedWallpaper = this->managedWallpaperSource(
        this->profile_.wallpaperSource, &wallpaperError);
    if (!wallpaperError.isEmpty())
    {
        QMessageBox::warning(this, u"Couldn't save theme"_s, wallpaperError);
        return false;
    }
    this->profile_.wallpaperSource = managedWallpaper;
    if (!writeThemeFile(this->selectedPath_,
                        buildCustomizedTheme(this->profile_)))
    {
        QMessageBox::warning(this, u"Couldn't save theme"_s,
                             u"That theme file couldn't be written."_s);
        return false;
    }
    this->savedProfile_ = this->profile_;
    this->cleanupManagedWallpapers();
    this->loadProfileIntoControls();
    getTheme()->reloadAvailableThemes();
    const bool active = getTheme()->themeName.getValue() == this->selectedKey_;
    if (active)
    {
        getTheme()->update();
    }
    if (apply)
    {
        if (!active)
        {
            getTheme()->themeName.setValue(this->selectedKey_);
        }
        getApp()->getWindows()->forceLayoutChannelViews();
        this->updateControlState();
        this->setStatus(u"Theme saved and applied."_s);
    }
    else
    {
        this->setStatus(u"Theme saved."_s);
    }
    return true;
}

void CustomizationPage::resetDraft()
{
    ++this->profileGeneration_;
    this->profile_ = this->savedProfile_;
    this->editedColorRoles_.clear();
    this->loadProfileIntoControls();
    this->setStatus(u"Changes reverted."_s);
}

void CustomizationPage::markColorEdited(const QString &role)
{
    if (role.isEmpty())
    {
        return;
    }
    if (role == u"chatText"_s)
    {
        this->profile_.separateChatText = true;
        const QSignalBlocker blocker(this->chatTextFollowsText_);
        this->chatTextFollowsText_->setChecked(false);
    }
    this->editedColorRoles_.insert(role);
}

void CustomizationPage::buildPalette()
{
    if (!this->selectedCustom_)
    {
        return;
    }

    this->readControlsIntoProfile();

    const auto generation = this->profileGeneration_;
    const auto originalProfile = this->profile_;
    QDialog dialog(this);
    dialog.setWindowTitle(u"Build a color palette"_s);
    dialog.setModal(true);
    dialog.setMinimumWidth(430);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *intro =
        new QLabel(u"Keep the checked colors and rebuild the rest."_s, &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *styleRow = new QHBoxLayout;
    styleRow->addWidget(new QLabel(u"Palette style:"_s, &dialog));
    auto *style = new QComboBox(&dialog);
    style->addItem(u"Dark"_s, static_cast<int>(ThemePaletteMode::Dark));
    style->addItem(u"Light"_s, static_cast<int>(ThemePaletteMode::Light));
    const bool preferLight =
        colorContrastRatio(Qt::black, this->profile_.background) >=
        colorContrastRatio(Qt::white, this->profile_.background);
    style->setCurrentIndex(preferLight ? 1 : 0);
    style->setToolTip(
        u"Style for the new colors. Kept colors stay unchanged."_s);
    styleRow->addWidget(style, 1);
    layout->addLayout(styleRow);

    auto *keptColors = new QGroupBox(u"Keep these colors"_s, &dialog);
    auto *keptGrid = new QGridLayout(keptColors);
    keptGrid->setHorizontalSpacing(18);
    keptGrid->setVerticalSpacing(5);
    struct KeepChoice {
        QString role;
        QCheckBox *box;
        bool ThemePaletteSelection::*member;
    };
    QVector<KeepChoice> choices;
    struct KeepDefinition {
        QString role;
        QString label;
        bool ThemePaletteSelection::*member;
    };
    const std::array definitions{
        KeepDefinition{u"background"_s, u"Background"_s,
                       &ThemePaletteSelection::background},
        KeepDefinition{u"chatBackground"_s, u"Chat background"_s,
                       &ThemePaletteSelection::chatBackground},
        KeepDefinition{u"surface"_s, u"Surface"_s,
                       &ThemePaletteSelection::surface},
        KeepDefinition{u"raisedSurface"_s, u"Raised surface"_s,
                       &ThemePaletteSelection::raisedSurface},
        KeepDefinition{u"accent"_s, u"Accent"_s,
                       &ThemePaletteSelection::accent},
    };
    const bool hasEditedColors =
        std::ranges::any_of(definitions, [this](const auto &definition) {
            return this->editedColorRoles_.contains(definition.role);
        });
    for (int index = 0; index < static_cast<int>(definitions.size()); ++index)
    {
        const auto &definition = definitions.at(index);
        auto *box = new QCheckBox(definition.label, keptColors);
        box->setChecked(hasEditedColors
                            ? this->editedColorRoles_.contains(definition.role)
                            : definition.role == u"background"_s);
        keptGrid->addWidget(box, index / 2, index % 2);
        choices.append({definition.role, box, definition.member});
    }
    layout->addWidget(keptColors);

    auto *note = new QLabel(u"Preview first, then save or revert."_s, &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *problem = new QLabel(&dialog);
    problem->setWordWrap(true);
    problem->setStyleSheet(u"color:#ff9b63;"_s);
    problem->hide();
    layout->addWidget(problem);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *cancel = new QPushButton(u"Cancel"_s, &dialog);
    auto *build = new QPushButton(u"Build palette"_s, &dialog);
    build->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(build);
    layout->addLayout(buttons);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    std::optional<ThemeCustomizationProfile> builtPalette;
    QSet<QString> keptRoles;
    QObject::connect(build, &QPushButton::clicked, &dialog, [&] {
        ThemePaletteSelection selection;
        keptRoles.clear();
        for (const auto &choice : choices)
        {
            if (!choice.box->isChecked())
            {
                continue;
            }
            selection.*choice.member = true;
            keptRoles.insert(choice.role);
        }
        if (!selection.any())
        {
            problem->setText(u"Choose at least one color to keep."_s);
            problem->show();
            return;
        }
        const auto mode =
            static_cast<ThemePaletteMode>(style->currentData().toInt());
        builtPalette = buildThemePalette(originalProfile, selection, mode);
        if (!builtPalette)
        {
            problem->setText(u"Choose at least one valid color to keep."_s);
            problem->show();
            return;
        }
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted || !builtPalette ||
        generation != this->profileGeneration_)
    {
        return;
    }

    this->profile_ = *builtPalette;
    this->editedColorRoles_ = keptRoles;
    this->loadProfileIntoControls();
    this->setStatus(
        keptRoles.size() == 1
            ? u"Palette rebuilt. Kept one color."_s
            : u"Palette rebuilt. Kept %1 colors."_s.arg(keptRoles.size()));
}

void CustomizationPage::exportThemeFile()
{
    const auto path = QFileDialog::getSaveFileName(
        this, u"Save theme"_s,
        safeFileBase(this->profile_.name) + u".moltheme.json"_s,
        u"Theme files (*.moltheme.json *.json)"_s);
    if (path.isEmpty())
    {
        return;
    }
    bool wallpaperRemoved = false;
    const auto theme = makeShareableCustomizedTheme(
        buildCustomizedTheme(this->profile_), &wallpaperRemoved);
    if (!writeThemeFile(path, appearanceEnvelope(theme)))
    {
        this->setStatus(u"That theme file couldn't be saved."_s, true);
        return;
    }
    this->setStatus(wallpaperRemoved ? u"Theme saved without its background."_s
                                     : u"Theme file saved."_s);
}

void CustomizationPage::importThemeFile()
{
    const auto path =
        QFileDialog::getOpenFileName(this, u"Open theme file"_s, {},
                                     u"Theme files (*.moltheme.json *.json)"_s);
    if (path.isEmpty())
    {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MAX_THEME_FILE_SIZE)
    {
        this->setStatus(u"That theme file couldn't be read."_s, true);
        return;
    }
    const auto bytes = file.read(MAX_THEME_FILE_SIZE + 1);
    if (bytes.size() > MAX_THEME_FILE_SIZE || !file.atEnd() ||
        file.error() != QFileDevice::NoError)
    {
        this->setStatus(u"That theme file couldn't be read."_s, true);
        return;
    }
    QString error;
    const auto imported =
        themeFromImport(bytes, importedThemeName(path), &error);
    if (!imported)
    {
        this->setStatus(error, true);
        return;
    }

    auto profile = imported->profile;
    const auto generation = this->profileGeneration_;
    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(u"Import theme"_s);
    prompt.setText(u"Import %1?"_s.arg(profile.name));
    auto details =
        u"Accent: %1"_s.arg(profile.accent.name(QColor::HexRgb).toUpper());
    if (imported->adapted)
    {
        details.prepend(u"Adapted from a Bluzyrino palette.\n"_s);
    }
    if (profile.hasWallpaper() || imported->localWallpaperRemoved)
    {
        details.append(u"\nIts background isn't included."_s);
    }
    prompt.setInformativeText(details);
    auto *add = prompt.addButton(u"Import"_s, QMessageBox::AcceptRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.exec();
    if (prompt.clickedButton() != add ||
        generation != this->profileGeneration_ || !this->confirmProfileChange())
    {
        this->setStatus(u"Theme wasn't imported."_s);
        return;
    }

    profile.name = profile.name.trimmed() + u" (Imported)"_s;
    const auto target = this->uniqueThemePath(profile.name);
    if (!QDir().mkpath(getTheme()->themesDirectory()) ||
        !writeThemeFile(target, buildCustomizedTheme(profile)))
    {
        this->setStatus(u"The imported theme couldn't be saved."_s, true);
        return;
    }
    this->reloadProfiles(QFileInfo(target).fileName());
    this->setStatus(
        imported->adapted
            ? u"Theme adapted and imported. Choose Apply theme to use it."_s
            : u"Theme imported. Choose Apply theme to use it."_s);
}

void CustomizationPage::setStatus(const QString &text, bool error)
{
    this->status_->setText(text);
    this->status_->setStyleSheet(error ? u"color:#ff7777;"_s : QString());
}

}  // namespace chatterino
