#pragma once

#include "singletons/ThemeCustomization.hpp"
#ifdef CHATTERINO_THEME_VIDEO
#    include "singletons/ThemeVideoCancellation.hpp"
#endif
#include "widgets/settingspages/SettingsPage.hpp"

#include <QSet>
#include <QVector>

#include <cstdint>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;

namespace chatterino {

class ThemePreviewWidget;
class TypographyPreviewWidget;
class WallpaperFocusWidget;
class ShadowPositionWidget;

/// Makes and edits the user's own themes. A theme is a regular theme file in
/// the Themes folder that also stores the few colors it was built from.
class CustomizationPage final : public SettingsPage
{
public:
    CustomizationPage();
    ~CustomizationPage() override;
    CustomizationPage(const CustomizationPage &) = delete;
    CustomizationPage &operator=(const CustomizationPage &) = delete;
    CustomizationPage(CustomizationPage &&) = delete;
    CustomizationPage &operator=(CustomizationPage &&) = delete;

    void onShow() override;
    bool filterElements(const QString &query) override;

private:
    void buildUi();
    QWidget *buildColorsTab();
    QWidget *buildChatTab();
    QWidget *buildInterfaceTab();
    QWidget *buildBackgroundTab();
    QWidget *buildTextTab();
    /// A color button with a hex field for a color that isn't part of the
    /// palette (borders, shadows).
    QWidget *makeColorControl(const QString &tooltip,
                              QColor ThemeCustomizationProfile::*member);
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

    void chooseWallpaper();
#ifdef CHATTERINO_THEME_VIDEO
    /// Converts a video (or animated GIF) into a background in a worker
    /// thread and selects it when done.
    void prepareVideoWallpaper(const QString &path);
    /// Asks how much of a long video to use. 0 for all of it, nothing if the
    /// user cancelled.
    std::optional<int> chooseVideoLength(double duration);
    /// Stops a running conversion.
    ThemeVideoStopSource wallpaperCancellation_;
#endif
    /// Bumped to drop the result of a conversion that is no longer wanted.
    std::uint64_t wallpaperGeneration_ = 0;
    QPushButton *cancelWallpaperButton_{};
    void clearWallpaper();
    /// Derives the palette from the colors of the background image.
    void suggestWallpaperPalette();
    /// Copies a background image into the Themes folder, so the theme keeps
    /// working when the original file is moved. Returns the path of the copy.
    QString managedWallpaperSource(const QString &source, QString *error) const;
    /// Removes copies of background images no theme uses anymore.
    void cleanupManagedWallpapers() const;

    void markColorEdited(const QString &role);
    void buildPalette();

    void exportThemeFile();
    void importThemeFile();
    /// Uploads the theme to the paste service and shows the code for it.
    void createThemeCode();
    void importThemeCode();
    /// Asks whether to import the theme, fetches its background if it has
    /// one, and adds it to the Themes folder.
    void installImportedTheme(ThemeCustomizationProfile profile, bool adapted,
                              bool localWallpaperRemoved);
    void writeImportedTheme(ThemeCustomizationProfile profile, bool adapted);
    /// Disables the share controls while something is up- or downloading.
    void setTransferBusy(bool busy);

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
    /// Bumped to drop the answers of transfers that are no longer wanted.
    std::uint64_t transferGeneration_ = 0;
    bool transferBusy_ = false;
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
    QPushButton *importFileButton_{};
    QPushButton *createCodeButton_{};
    QPushButton *importCodeButton_{};
    QCheckBox *shareWallpaper_{};
    QComboBox *shareExpiry_{};
    QLineEdit *shareCode_{};
    QLineEdit *importCode_{};
    ThemePreviewWidget *preview_{};
    QWidget *colorsEditor_{};
    QWidget *chatEditor_{};
    QWidget *interfaceEditor_{};
    QWidget *backgroundEditor_{};
    QWidget *textEditor_{};
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

    QCheckBox *alternateMessageRows_{};
    QSlider *alternateMessageOpacity_{};
    QSlider *alternateMessageContrast_{};
    QSlider *highlightOpacityAdjustment_{};
    QCheckBox *roundChat_{};
    QSlider *chatCornerRadius_{};
    QCheckBox *chatBorder_{};
    QWidget *chatBorderColor_{};
    QSlider *chatBorderWidth_{};
    QSlider *chatBorderOpacity_{};
    QCheckBox *messageShadow_{};
    QCheckBox *messageShadowEmotes_{};
    QWidget *messageShadowColor_{};
    QSlider *messageShadowOpacity_{};
    ShadowPositionWidget *messageShadowPosition_{};
    QPushButton *messageShadowCenter_{};
    QSlider *messageShadowBlur_{};

    QLineEdit *wallpaperPath_{};
    QComboBox *wallpaperMode_{};
    QSlider *wallpaperZoom_{};
    QSlider *wallpaperOpacity_{};
    QWidget *wallpaperOverlayColor_{};
    QSlider *wallpaperOverlayOpacity_{};
    QSlider *wallpaperBlur_{};
    WallpaperFocusWidget *wallpaperFocus_{};
    QPushButton *wallpaperCenterFocus_{};
    QPushButton *wallpaperColors_{};

    QCheckBox *useThemeFonts_{};
    QCheckBox *useThemeFontSizes_{};
    QComboBox *chatFont_{};
    QSlider *chatFontSize_{};
    QSlider *chatFontWeight_{};
    QComboBox *usernameFont_{};
    QSlider *usernameFontSize_{};
    QSlider *usernameFontWeight_{};
    QComboBox *interfaceFont_{};
    QSlider *interfaceFontSize_{};
    TypographyPreviewWidget *typographyPreview_{};

    QComboBox *foundation_{};
    QComboBox *tabShape_{};
    QSlider *tabCornerRadius_{};
    QSlider *tabSpacing_{};
};

}  // namespace chatterino
