#pragma once

#include "singletons/ThemeCustomization.hpp"
#include "widgets/settingspages/SettingsPage.hpp"

#include <QSet>
#include <QVector>

#include <cstdint>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;

namespace chatterino {

class ThemePreviewWidget;

/// Makes and edits the user's own themes. A theme is a regular theme file in
/// the Themes folder that also stores the few colors it was built from.
class CustomizationPage final : public SettingsPage
{
public:
    CustomizationPage();

    void onShow() override;
    bool filterElements(const QString &query) override;

private:
    void buildUi();
    QWidget *buildColorsTab();
    QWidget *buildShareTab();

    void reloadProfiles(const QString &preferredKey = {});
    /// Asks what to do with unsaved changes. False if the user cancelled.
    bool confirmProfileChange();
    void loadSelectedProfile();
    void loadProfileIntoControls();
    void readControlsIntoProfile();
    void updatePreview();
    void updateControlState();
    void updateReadability();

    void applySelectedTheme();
    void duplicateProfile();
    void renameProfile();
    void deleteProfile();
    bool saveCustomProfile(bool apply);
    void resetDraft();

    void markColorEdited(const QString &role);
    void buildPalette();

    void exportThemeFile();
    void importThemeFile();

    QString uniqueThemePath(const QString &name) const;
    void setStatus(const QString &text, bool error = false);

    ThemeCustomizationProfile profile_;
    ThemeCustomizationProfile savedProfile_;
    QString selectedKey_;
    QString selectedPath_;
    bool selectedCustom_ = false;
    bool loadingControls_ = false;
    /// Bumped whenever another theme is loaded, so dialogs that were open
    /// meanwhile don't change the wrong theme.
    std::uint64_t profileGeneration_ = 0;
    QSet<QString> editedColorRoles_;

    QComboBox *profileBox_{};
    QLabel *profileKind_{};
    QPushButton *useButton_{};
    QPushButton *duplicateButton_{};
    QPushButton *renameButton_{};
    QPushButton *deleteButton_{};
    QPushButton *saveButton_{};
    QPushButton *resetButton_{};
    QPushButton *exportFileButton_{};
    ThemePreviewWidget *preview_{};
    QWidget *colorsEditor_{};
    QLabel *readability_{};
    QLabel *status_{};
    QCheckBox *chatTextFollowsText_{};

    struct ColorControl {
        QPushButton *button{};
        QLineEdit *input{};
        QColor ThemeCustomizationProfile::*member{};
    };
    QVector<ColorControl> colorControls_;

    QSlider *panelContrast_{};
    QComboBox *accentStrength_{};
    QSlider *inactiveTabContrast_{};
    QSlider *channelBarContrast_{};
};

}  // namespace chatterino
