#pragma once

#include <QWidget>

class QLabel;
class QCheckBox;
class QComboBox;

namespace chatterino {

class GeneralPageView;
class DescriptionLabel;
struct DropdownArgs;

/// The settings that came from Technorino, shown on the JilChat Chatterino page
class TechnorinoPage : public QWidget
{
    Q_OBJECT

public:
    explicit TechnorinoPage(GeneralPageView &view);

private:
    void initLayout(GeneralPageView &layout);
    void initExtra();

    QString getFont(const DropdownArgs &args) const;

    DescriptionLabel *cachePath_{};
};

}  // namespace chatterino
