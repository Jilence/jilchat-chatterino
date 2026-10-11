#include "widgets/settingspages/CustomizationPage.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
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

QWidget *makeSlider(int minimum, int maximum, QSlider **slider,
                    const QString &suffix = {})
{
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    *slider = new QSlider(Qt::Horizontal, container);
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

}  // namespace

/// A small mock-up of a window with the colors of the theme being edited.
class ThemePreviewWidget final : public QWidget
{
public:
    explicit ThemePreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setMinimumHeight(260);
    }

    void setProfile(const ThemeCustomizationProfile &profile)
    {
        this->profile_ = profile;
        this->colors_ =
            buildCustomizedTheme(profile).value(u"colors"_s).toObject();
        this->update();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        const auto &c = this->colors_;
        const QColor fallback = this->palette().color(QPalette::Window);
        const QColor fallbackText = this->palette().color(QPalette::WindowText);

        const auto windowBackground =
            nestedColor(c, {u"window"_s, u"background"_s}, fallback);
        const auto chatBackground =
            nestedColor(c, {u"splits"_s, u"background"_s}, fallback);
        const auto regularRow = nestedColor(
            c, {u"messages"_s, u"backgrounds"_s, u"regular"_s}, chatBackground);
        const auto alternateRow = nestedColor(
            c, {u"messages"_s, u"backgrounds"_s, u"alternate"_s}, regularRow);
        const auto textColor = [&](const QString &name) {
            return nestedColor(c, {u"messages"_s, u"textColors"_s, name},
                               fallbackText);
        };
        const auto tabColor = [&](const QString &tab, const QString &group,
                                  const QString &name) {
            return nestedColor(c, {u"tabs"_s, tab, group, name}, fallback);
        };
        const bool alternate =
            this->profile_.useThemeMessageRows
                ? this->profile_.alternateMessageRows
                : getSettings()->alternateMessages.getValue();
        const bool individualTabs =
            this->profile_.tabShape == ThemeTabShape::Individual;
        const int tabGap =
            individualTabs ? std::clamp(this->profile_.tabSpacing, 0, 4) : 0;
        const qreal tabRadius =
            std::clamp(this->profile_.tabCornerRadius, 0, 10);

        painter.fillRect(this->rect(), windowBackground);

        const auto metrics = this->fontMetrics();
        const int pad = 8;
        const int tabHeight = metrics.height() + 10;
        const int rowHeight = metrics.height() + 8;

        // tabs
        struct Tab {
            QString name;
            QString key;
        };
        const std::array tabs{
            Tab{.name = u"channel"_s, .key = u"selected"_s},
            Tab{.name = u"other"_s, .key = u"regular"_s},
            Tab{.name = u"unread"_s, .key = u"newMessage"_s},
            Tab{.name = u"mention"_s, .key = u"highlighted"_s},
        };
        int x = pad;
        for (const auto &tab : tabs)
        {
            const int width = metrics.horizontalAdvance(tab.name) + 20;
            const QRect rect(x, pad, width, tabHeight);
            const bool first = &tab == &tabs.front();
            const bool last = &tab == &tabs.back();
            const auto shape = themeTopTabPath(QRectF(rect), tabRadius,
                                               individualTabs || first,
                                               individualTabs || last);
            painter.setRenderHint(QPainter::Antialiasing, tabRadius > 0);
            painter.fillPath(shape,
                             tabColor(tab.key, u"backgrounds"_s, u"regular"_s));
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.save();
            painter.setClipPath(shape);
            painter.fillRect(QRect(x, pad, width, 2),
                             tabColor(tab.key, u"line"_s, u"regular"_s));
            painter.restore();
            painter.setPen(
                nestedColor(c, {u"tabs"_s, tab.key, u"text"_s}, fallbackText));
            painter.drawText(rect, Qt::AlignCenter, tab.name);
            x += width + tabGap;
        }

        // the split below the tabs
        const QRect split(pad, pad + tabHeight, this->width() - 2 * pad,
                          this->height() - 2 * pad - tabHeight);
        painter.fillRect(split, chatBackground);

        const QRect header(split.left(), split.top(), split.width(), rowHeight);
        painter.fillRect(
            header, nestedColor(c, {u"splits"_s, u"header"_s, u"background"_s},
                                fallback));
        painter.setPen(nestedColor(c, {u"splits"_s, u"header"_s, u"text"_s},
                                   fallbackText));
        painter.drawText(header.adjusted(pad, 0, -pad, 0),
                         Qt::AlignVCenter | Qt::AlignLeft, u"channel"_s);
        painter.setPen(
            nestedColor(c, {u"splits"_s, u"header"_s, u"border"_s}, fallback));
        painter.drawLine(header.bottomLeft(), header.bottomRight());

        const QRect input(split.left(), split.bottom() - rowHeight - 3,
                          split.width(), rowHeight + 4);
        painter.fillRect(
            input, nestedColor(c, {u"splits"_s, u"input"_s, u"background"_s},
                               fallback));
        painter.setPen(textColor(u"chatPlaceholder"_s));
        painter.drawText(input.adjusted(pad, 0, -pad, 0),
                         Qt::AlignVCenter | Qt::AlignLeft, u"Send message"_s);

        // messages
        struct Part {
            QString text;
            QColor color;
        };
        const auto timestamp = textColor(u"timestamp"_s);
        const auto regular = textColor(u"regular"_s);
        const auto accent = nestedColor(c, {u"accent"_s}, fallbackText);
        const std::vector<std::vector<Part>> rows{
            std::vector<Part>{
                Part{.text = u"12:01"_s, .color = timestamp},
                Part{.text = u"viewer:"_s, .color = QColor(0x1e, 0x90, 0xff)},
                Part{.text = u"hello chat"_s, .color = regular},
            },
            std::vector<Part>{
                Part{.text = u"12:02"_s, .color = timestamp},
                Part{.text = u"someone:"_s, .color = QColor(0xff, 0x69, 0xb4)},
                Part{.text = u"look at"_s, .color = regular},
                Part{.text = u"twitch.tv"_s, .color = textColor(u"link"_s)},
            },
            std::vector<Part>{
                Part{.text = u"12:02"_s, .color = timestamp},
                Part{.text = u"joined channel"_s,
                     .color = textColor(u"system"_s)},
            },
            std::vector<Part>{
                Part{.text = u"12:03"_s, .color = timestamp},
                Part{.text = u"mod:"_s, .color = accent},
                Part{.text = u"welcome everyone"_s, .color = regular},
            },
        };
        painter.setClipRect(QRect(split.left(), header.bottom() + 1,
                                  split.width(),
                                  input.top() - header.bottom() - 1));
        int y = header.bottom() + 1;
        for (size_t index = 0; index < rows.size(); ++index)
        {
            const QRect row(split.left(), y, split.width(), rowHeight);
            painter.fillRect(
                row, alternate && index % 2 == 1 ? alternateRow : regularRow);
            int textX = row.left() + pad;
            for (const auto &part : rows.at(index))
            {
                painter.setPen(part.color);
                painter.drawText(
                    QRect(textX, row.top(), row.width(), row.height()),
                    Qt::AlignVCenter | Qt::AlignLeft, part.text);
                textX += metrics.horizontalAdvance(part.text) + 5;
            }
            y += rowHeight;
        }
        painter.setClipping(false);

        if (this->profile_.roundChat && this->profile_.cornerRadius() > 0)
        {
            const QRectF area(split.left(), header.bottom() + 1, split.width(),
                              input.top() - header.bottom() - 1);
            paintThemeChatFrame(
                painter, area, this->profile_.cornerRadius(),
                this->profile_.chatBorder
                    ? std::clamp(this->profile_.chatBorderWidth, 1, 4)
                    : 0,
                nestedColor(c, {u"splits"_s, u"header"_s, u"background"_s},
                            fallback),
                nestedColor(c, {u"splits"_s, u"input"_s, u"background"_s},
                            fallback),
                this->profile_.chatBorderColor,
                this->profile_.chatBorder ? this->profile_.chatBorderOpacity
                                          : 0);
        }
    }

private:
    QJsonObject colors_;
    ThemeCustomizationProfile profile_;
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
        makeSlider(-100, 100, &this->highlightOpacityAdjustment_, u"%"_s));
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
    this->messageShadow_->setToolTip(
        u"Shown in chat, not in the preview. Animated emotes don't get a "
        u"shadow."_s);
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
    shadowForm->addRow(u"Move right:"_s,
                       makeSlider(-8, 8, &this->messageShadowOffsetX_));
    shadowForm->addRow(u"Move down:"_s,
                       makeSlider(-8, 8, &this->messageShadowOffsetY_));
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
          this->messageShadowOpacity_, this->messageShadowOffsetX_,
          this->messageShadowOffsetY_, this->messageShadowBlur_})
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

QWidget *CustomizationPage::buildShareTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    auto *description = new QLabel(
        u"Save the selected theme as a file to share it, or import a theme "
        u"file. Theme files from Moltorino and Bluzyrino can be imported "
        u"too; their backgrounds and fonts are kept in the theme but aren't "
        u"shown here yet."_s);
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
    this->messageShadowOffsetX_->setValue(this->profile_.messageShadowOffsetX);
    this->messageShadowOffsetY_->setValue(this->profile_.messageShadowOffsetY);
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
    this->profile_.messageShadowOffsetX = this->messageShadowOffsetX_->value();
    this->profile_.messageShadowOffsetY = this->messageShadowOffsetY_->value();
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
    this->messageShadowOffsetX_->setEnabled(shadow);
    this->messageShadowOffsetY_->setEnabled(shadow);
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
    this->setStatus(u"Theme deleted."_s);
}

bool CustomizationPage::saveCustomProfile(bool apply)
{
    if (!this->selectedCustom_)
    {
        return false;
    }
    this->readControlsIntoProfile();
    if (!writeThemeFile(this->selectedPath_,
                        buildCustomizedTheme(this->profile_)))
    {
        QMessageBox::warning(this, u"Couldn't save theme"_s,
                             u"That theme file couldn't be written."_s);
        return false;
    }
    this->savedProfile_ = this->profile_;
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
        details.append(u"\nIts background isn't shown here yet."_s);
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
