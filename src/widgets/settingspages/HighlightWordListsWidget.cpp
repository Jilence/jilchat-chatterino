// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/HighlightWordListsWidget.hpp"

#include "controllers/highlights/HighlightChannelScope.hpp"
#include "controllers/highlights/HighlightMatch.hpp"
#include "controllers/highlights/HighlightWordList.hpp"
#include "controllers/highlights/types/YourUsernameHighlight.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>
#include <optional>

namespace chatterino {

namespace {

void alignFormLabels(QFormLayout *layout, const QWidget *reference,
                     std::initializer_list<QWidget *> fields)
{
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->setHorizontalSpacing(10);
    layout->setVerticalSpacing(8);

    const auto labelWidth =
        reference->fontMetrics().horizontalAdvance("Message background:") + 8;
    for (auto *field : fields)
    {
        if (auto *label = layout->labelForField(field))
        {
            label->setFixedWidth(labelWidth);
        }
    }
}

struct ParsedWordListTerms {
    std::vector<QString> terms;
    qsizetype blanks{};
    qsizetype duplicates{};
    qsizetype oversized{};
    qsizetype overLimit{};
    qsizetype overCharacterBudget{};
    qsizetype invalidRegex{};
    QString firstRegexError;
};

/// The most text a list of rules is read from: 4 MiB
constexpr qsizetype WORD_LIST_MAX_SOURCE_CHARACTERS = 4 * 1024 * 1024;

ParsedWordListTerms parseWordListTerms(const QString &text, bool caseSensitive,
                                       bool isRegex)
{
    ParsedWordListTerms parsed;
    if (text.size() > WORD_LIST_MAX_SOURCE_CHARACTERS)
    {
        parsed.overCharacterBudget = 1;
        return parsed;
    }
    QSet<QString> seen;
    const auto maxTerms =
        static_cast<qsizetype>(isRegex ? HIGHLIGHT_WORD_LIST_MAX_REGEX_RULES
                                       : HIGHLIGHT_WORD_LIST_MAX_LITERAL_RULES);
    const auto maxLength = static_cast<qsizetype>(
        isRegex ? HIGHLIGHT_WORD_LIST_MAX_REGEX_RULE_LENGTH
                : HIGHLIGHT_WORD_LIST_MAX_LITERAL_RULE_LENGTH);
    const auto regexOptions =
        QRegularExpression::UseUnicodePropertiesOption |
        (caseSensitive ? QRegularExpression::NoPatternOption
                       : QRegularExpression::CaseInsensitiveOption);
    qsizetype lineNumber = 0;
    qsizetype acceptedCharacters = 0;
    for (auto line : text.split(u'\n', Qt::KeepEmptyParts))
    {
        ++lineNumber;
        if (line.endsWith(u'\r'))
        {
            line.chop(1);
        }
        line = line.trimmed();
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark)
        {
            line.remove(0, 1);
        }
        if (line.isEmpty())
        {
            ++parsed.blanks;
            continue;
        }
        if (line.size() > maxLength)
        {
            ++parsed.oversized;
            continue;
        }
        const auto key = isRegex || caseSensitive ? line : line.toCaseFolded();
        if (seen.contains(key))
        {
            ++parsed.duplicates;
            continue;
        }
        if (static_cast<qsizetype>(parsed.terms.size()) >= maxTerms)
        {
            ++parsed.overLimit;
            continue;
        }
        if (acceptedCharacters + line.size() >
            static_cast<qsizetype>(HIGHLIGHT_WORD_LIST_MAX_RULE_CHARACTERS))
        {
            ++parsed.overCharacterBudget;
            continue;
        }
        if (isRegex)
        {
            const QRegularExpression regex(line, regexOptions);
            if (!regex.isValid())
            {
                ++parsed.invalidRegex;
                if (parsed.firstRegexError.isEmpty())
                {
                    parsed.firstRegexError = QString("Line %1: %2")
                                                 .arg(lineNumber)
                                                 .arg(regex.errorString());
                }
                continue;
            }
        }
        seen.insert(key);
        acceptedCharacters += line.size();
        parsed.terms.emplace_back(std::move(line));
    }
    return parsed;
}

QString normalizeImportedWordListText(const QString &text,
                                      bool commasSeparateRules)
{
    if (!commasSeparateRules)
    {
        return text;
    }

    QStringList terms;
    QString current;
    bool quoted = false;
    const auto flush = [&] {
        auto term = current.trimmed();
        if (!term.isEmpty())
        {
            terms.append(std::move(term));
        }
        current.clear();
    };
    for (qsizetype index = 0; index < text.size(); ++index)
    {
        const auto ch = text.at(index);
        if (ch == u'"')
        {
            if (quoted && index + 1 < text.size() && text.at(index + 1) == u'"')
            {
                current.append(u'"');
                ++index;
            }
            else
            {
                quoted = !quoted;
            }
        }
        else if (!quoted && (ch == u',' || ch == u'\n' || ch == u'\r'))
        {
            flush();
        }
        else if (quoted && (ch == u'\n' || ch == u'\r'))
        {
            if (!current.isEmpty() && !current.endsWith(u' '))
            {
                current.append(u' ');
            }
        }
        else
        {
            current.append(ch);
        }
    }
    flush();
    return terms.join(u'\n');
}

/// Whether a file of rules, one per line, is meant as regular expressions:
/// every line has to be one, and enough of them have to use their syntax.
bool looksLikeRegularExpressionList(const QString &text)
{
    static const QRegularExpression strongSyntax(QStringLiteral(
        R"((?:\\[AbBdDsSwWzZpP]|\\[.^$|?*+(){}\[\]\\]|\(\?|\[[^\]\r\n]+\]|\{\d+(?:,\d*)?\}|\.\*|\.\+|\|))"));
    const auto options = QRegularExpression::UseUnicodePropertiesOption;
    qsizetype rules = 0;
    qsizetype strongRules = 0;
    for (auto line : text.split(u'\n', Qt::SkipEmptyParts))
    {
        line = line.trimmed();
        if (line.isEmpty())
        {
            continue;
        }
        ++rules;
        const QRegularExpression regex(line, options);
        if (!regex.isValid())
        {
            return false;
        }
        const bool anchored = line.startsWith(u'^') || line.endsWith(u'$');
        if (anchored || strongSyntax.match(line).hasMatch())
        {
            ++strongRules;
        }
    }
    return rules > 0 && strongRules > 0 && strongRules * 3 >= rules;
}

struct ImportedWordListFile {
    QString text;
    QString suggestedName;
    bool chattySyntax{};
    bool suggestedRegex{};
};

std::optional<ImportedWordListFile> openWordListFile(QWidget *parent,
                                                     const QString &title)
{
    const QPointer<QWidget> guardedParent(parent);
    const auto fileName = QFileDialog::getOpenFileName(
        parent, title, QString(),
        "Moderation list files (*.txt *.list *.csv);;Text files (*.txt "
        "*.list);;CSV files (*.csv);;All files (*)");
    if (!guardedParent || fileName.isEmpty())
    {
        return std::nullopt;
    }

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(
            parent, "Could not import list",
            "JilChat Chatterino could not open the selected file.");
        return std::nullopt;
    }
    if (file.size() > 4 * 1024 * 1024)
    {
        QMessageBox::warning(parent, "Moderation list is too large",
                             "The maximum import size is 4 MiB.");
        return std::nullopt;
    }

    const auto rawText = QString::fromUtf8(file.readAll());
    const bool chattySyntax = false;
    const bool suggestedRegex =
        !chattySyntax && looksLikeRegularExpressionList(rawText);
    auto commasSeparateRules =
        !chattySyntax && !suggestedRegex &&
        QFileInfo(fileName).suffix().compare("csv", Qt::CaseInsensitive) == 0;
    if (!chattySyntax && !commasSeparateRules && rawText.contains(u','))
    {
        const auto choice = QMessageBox::question(
            parent, "Choose import format",
            "Use commas to separate rules?\n\nChoose No to keep each line "
            "as one rule.",
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::No);
        if (choice == QMessageBox::Cancel)
        {
            return std::nullopt;
        }
        commasSeparateRules = choice == QMessageBox::Yes;
    }

    if (!guardedParent)
    {
        return std::nullopt;
    }
    return ImportedWordListFile{
        chattySyntax
            ? rawText
            : normalizeImportedWordListText(rawText, commasSeparateRules),
        QFileInfo(fileName).completeBaseName().trimmed(),
        chattySyntax,
        suggestedRegex,
    };
}

std::vector<QString> parseChannels(const QString &text)
{
    std::vector<QString> channels;
    QSet<QString> seen;
    for (auto channel :
         text.split(QRegularExpression(QStringLiteral("[,\\r\\n]")),
                    Qt::SkipEmptyParts))
    {
        auto target = makeHighlightChannelTarget(
            HighlightChannelTargetPlatform::Any, std::move(channel));
        if (!target)
        {
            continue;
        }

        auto key = normalizeHighlightChannelName(target->channel);
        if (seen.contains(key))
        {
            continue;
        }
        seen.insert(std::move(key));
        channels.emplace_back(std::move(target->channel));
    }
    return channels;
}

/// Shows what a marker looks like on a piece of text
class MatchAppearancePreview final : public QWidget
{
public:
    explicit MatchAppearancePreview(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setAppearance(QColor color, HighlightMatchStyle style)
    {
        this->color_ = color;
        this->style_ = style;
        this->update();
    }

    /// Room for the whole example. This is asked for when the dialog is laid
    /// out, with the font the widget has by then.
    QSize minimumSizeHint() const override
    {
        const auto metrics = this->fontMetrics();
        return {
            metrics.horizontalAdvance("Example message with matched text") + 32,
            std::max(42, metrics.height() + 16)};
    }

    QSize sizeHint() const override
    {
        return this->minimumSizeHint();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(this->rect(), this->palette().base());

        const QString prefix = "Example message with ";
        const QString matched = "matched text";
        const auto metrics = this->fontMetrics();
        const auto baseline =
            (this->height() + metrics.ascent() - metrics.descent()) / 2;
        const auto left = 10;
        painter.setFont(this->font());
        painter.setPen(this->palette().text().color());
        painter.drawText(left, baseline, prefix);

        const QRectF markerRect(left + metrics.horizontalAdvance(prefix) - 2,
                                (this->height() - metrics.height()) / 2.0,
                                metrics.horizontalAdvance(matched) + 4,
                                metrics.height());
        const FragmentHighlight highlight{
            .match = {.color = this->color_, .style = this->style_},
        };
        paintFragmentHighlightBackground(painter, markerRect, highlight);
        painter.setPen(this->palette().text().color());
        painter.drawText(QPointF(markerRect.left() + 2, baseline), matched);
        paintFragmentHighlightForeground(painter, markerRect, highlight);
    }

private:
    QColor color_{Qt::red};
    HighlightMatchStyle style_{HighlightMatchStyle::Outline};
};

/// The style and color of a marker with a preview, as part of a dialog
class MatchAppearanceControls final : public QWidget
{
public:
    MatchAppearanceControls(QColor color, HighlightMatchStyle style,
                            QWidget *parent = nullptr)
        : QWidget(parent)
        , color_(color)
    {
        auto *layout = new QFormLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        this->style_ = new QComboBox(this);
        for (const auto candidate :
             {HighlightMatchStyle::None, HighlightMatchStyle::Outline,
              HighlightMatchStyle::Fill, HighlightMatchStyle::OutlineAndFill,
              HighlightMatchStyle::Underline})
        {
            this->style_->addItem(highlightMatchStyleName(candidate),
                                  static_cast<int>(candidate));
        }
        const auto styleIndex = this->style_->findData(static_cast<int>(style));
        this->style_->setCurrentIndex(styleIndex >= 0 ? styleIndex : 1);
        layout->addRow("Marker style:", this->style_);

        this->colorButton_ = new QPushButton(this);
        layout->addRow("Marker color:", this->colorButton_);

        this->preview_ = new MatchAppearancePreview(this);
        layout->addRow("Preview:", this->preview_);

        QObject::connect(this->style_, &QComboBox::currentIndexChanged, this,
                         [this] {
                             this->updateState();
                         });
        QObject::connect(
            this->colorButton_, &QPushButton::clicked, this, [this] {
                auto *picker = new ColorPickerDialog(this->color_, this);
                QObject::connect(picker, &ColorPickerDialog::colorConfirmed,
                                 this, [this](const QColor &color) {
                                     if (color.isValid())
                                     {
                                         this->color_ = color;
                                         this->updateState();
                                     }
                                 });
                picker->show();
            });
        this->updateState();
    }

    QColor color() const
    {
        return this->color_;
    }

    HighlightMatchStyle style() const
    {
        return static_cast<HighlightMatchStyle>(
            this->style_->currentData().toInt());
    }

private:
    void updateState()
    {
        this->colorButton_->setText(this->color_.name(QColor::HexArgb));
        QPixmap swatch(16, 16);
        swatch.fill(this->color_);
        this->colorButton_->setIcon(QIcon(swatch));
        // without a marker there is nothing to color
        this->colorButton_->setEnabled(this->style() !=
                                       HighlightMatchStyle::None);
        this->preview_->setAppearance(this->color_, this->style());
    }

    QColor color_;
    QComboBox *style_{};
    QPushButton *colorButton_{};
    MatchAppearancePreview *preview_{};
};

/// Asks for a list of rules.
class WordListEditorDialog final : public QDialog
{
public:
    WordListEditorDialog(QWidget *parent,
                         const std::optional<HighlightWordList> &existing,
                         QString importedText = {}, QString importedName = {},
                         bool importedRegex = false)
        : QDialog(parent)
    {
        this->setWindowTitle(existing ? "Edit moderation list"
                                      : "Add moderation list");
        this->setMinimumSize(720, 680);

        auto *layout = new QVBoxLayout(this);
        layout->setSpacing(10);
        auto *details = new QGroupBox("List details", this);
        auto *form = new QFormLayout(details);
        this->name_ = new QLineEdit(this);
        this->name_->setPlaceholderText("Example: Channel moderation terms");
        form->addRow("Name:", this->name_);

        this->channels_ = new QLineEdit(this);
        this->channels_->setPlaceholderText(
            "All channels (or channel names separated by commas)");
        this->channels_->setToolTip("Leave empty for every channel.");
        form->addRow("Applies in:", this->channels_);

        this->platform_ = new QComboBox(this);
        this->platform_->addItem(
            "All platforms", static_cast<int>(HighlightWordListPlatform::All));
        this->platform_->addItem(
            "Twitch", static_cast<int>(HighlightWordListPlatform::Twitch));
        this->platform_->addItem(
            "YouTube", static_cast<int>(HighlightWordListPlatform::YouTube));
        this->platform_->addItem(
            "Kick", static_cast<int>(HighlightWordListPlatform::Kick));
        this->platform_->setToolTip("Limit this list to one platform.");
        form->addRow("Platform:", this->platform_);

        this->enabled_ = new QCheckBox("Enabled", this);
        this->enabled_->setChecked(true);
        form->addRow("Status:", this->enabled_);
        alignFormLabels(
            form, this,
            {this->name_, this->channels_, this->platform_, this->enabled_});
        layout->addWidget(details);

        auto *rules = new QGroupBox("Rules", this);
        auto *rulesLayout = new QVBoxLayout(rules);
        auto *ruleOptionsWidget = new QWidget(rules);
        auto *ruleOptions = new QHBoxLayout(ruleOptionsWidget);
        ruleOptions->setContentsMargins(0, 0, 0, 0);
        this->regex_ = new QCheckBox("Enable regex", this);
        this->regex_->setToolTip("Treat each line as a regular expression.");
        this->caseSensitive_ = new QCheckBox("Case sensitive", this);
        ruleOptions->addWidget(this->regex_);
        ruleOptions->addWidget(this->caseSensitive_);
        ruleOptions->addStretch(1);
        auto *ruleOptionsForm = new QFormLayout();
        ruleOptionsForm->addRow("Match options:", ruleOptionsWidget);
        alignFormLabels(ruleOptionsForm, this, {ruleOptionsWidget});
        rulesLayout->addLayout(ruleOptionsForm);
        this->intro_ = new QLabel(this);
        this->intro_->setWordWrap(true);
        rulesLayout->addWidget(this->intro_);
        this->terms_ = new QPlainTextEdit(this);
        this->terms_->setPlaceholderText("bad phrase\nanother phrase");
        rulesLayout->addWidget(this->terms_, 1);
        this->summary_ = new QLabel(this);
        this->summary_->setWordWrap(true);
        auto *rulesFooter = new QHBoxLayout();
        rulesFooter->setContentsMargins(0, 0, 0, 0);
        rulesFooter->addWidget(this->summary_, 1);
        auto *importRules = new QPushButton("Import rules from file", rules);
        importRules->setMinimumWidth(
            std::max(150, importRules->sizeHint().width()));
        importRules->setToolTip("Import rules from a TXT, LIST, or CSV file.");
        rulesFooter->addWidget(importRules);
        rulesLayout->addLayout(rulesFooter);
        layout->addWidget(rules, 1);

        auto *alerts = new QGroupBox("Notifications", this);
        auto *alertsLayout = new QFormLayout(alerts);
        auto *alertOptionsWidget = new QWidget(alerts);
        auto *alertOptions = new QGridLayout(alertOptionsWidget);
        alertOptions->setContentsMargins(0, 0, 0, 0);
        alertOptions->setHorizontalSpacing(16);
        this->showInMentions_ = new QCheckBox("Show in Mentions", this);
        this->alert_ = new QCheckBox("Flash taskbar", this);
        this->sound_ = new QCheckBox("Play sound", this);
        alertOptions->addWidget(this->showInMentions_, 0, 0);
        alertOptions->addWidget(this->alert_, 0, 1);
        alertOptions->addWidget(this->sound_, 0, 2);
        alertOptions->setColumnStretch(0, 1);
        alertOptions->setColumnStretch(1, 1);
        alertOptions->setColumnStretch(2, 1);
        alertsLayout->addRow("Actions:", alertOptionsWidget);

        auto *soundWidget = new QWidget(alerts);
        auto *soundLayout = new QHBoxLayout(soundWidget);
        soundLayout->setContentsMargins(0, 0, 0, 0);
        soundLayout->setSpacing(8);
        this->customSoundButton_ = new QPushButton(this);
        this->clearSoundButton_ = new QPushButton("Clear", this);
        soundLayout->addWidget(this->customSoundButton_, 1);
        soundLayout->addWidget(this->clearSoundButton_);
        alertsLayout->addRow("Sound:", soundWidget);
        alignFormLabels(alertsLayout, this, {alertOptionsWidget, soundWidget});
        layout->addWidget(alerts);

        auto *appearance = new QGroupBox("Appearance", this);
        auto *appearanceLayout = new QVBoxLayout(appearance);
        auto *colorRow = new QFormLayout();
        this->colorButton_ = new QPushButton(this);
        colorRow->addRow("Message background:", this->colorButton_);
        alignFormLabels(colorRow, this, {this->colorButton_});
        appearanceLayout->addLayout(colorRow);

        this->color_ =
            highlights::YourUsernameHighlight::BACKGROUND_COLOR_DEFAULT;
        auto matchColor = defaultNewHighlightMatchColor();
        auto matchStyle = HighlightMatchStyle::Outline;
        if (existing)
        {
            this->name_->setText(existing->name());
            QStringList channels;
            for (const auto &channel : existing->channels())
            {
                channels.append(channel);
            }
            this->channels_->setText(channels.join(", "));
            const auto platformIndex = this->platform_->findData(
                static_cast<int>(existing->platform()));
            this->platform_->setCurrentIndex(std::max(platformIndex, 0));
            QStringList terms;
            for (const auto &term : existing->terms())
            {
                terms.append(term);
            }
            this->terms_->setPlainText(terms.join(u'\n'));
            this->enabled_->setChecked(existing->enabled());
            this->caseSensitive_->setChecked(existing->caseSensitive());
            this->showInMentions_->setChecked(existing->showInMentions());
            this->alert_->setChecked(existing->hasAlert());
            this->sound_->setChecked(existing->hasSound());
            this->soundUrl_ = existing->soundUrl().toString();
            this->color_ = *existing->color();
            this->regex_->setChecked(existing->isRegex());
            matchColor = *existing->matchColor();
            matchStyle = existing->matchStyle();
        }
        else
        {
            this->name_->setText(std::move(importedName));
            this->terms_->setPlainText(std::move(importedText));
            this->regex_->setChecked(importedRegex);
            if (importedRegex)
            {
                this->caseSensitive_->setChecked(true);
            }
        }

        this->matchAppearance_ =
            new MatchAppearanceControls(matchColor, matchStyle, appearance);
        appearanceLayout->addWidget(this->matchAppearance_);
        layout->addWidget(appearance);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        this->updateSummary();

        auto *analysisTimer = new QTimer(this);
        analysisTimer->setSingleShot(true);
        analysisTimer->setInterval(250);
        QObject::connect(analysisTimer, &QTimer::timeout, this, [this] {
            this->updateSummary();
        });
        QObject::connect(this->terms_, &QPlainTextEdit::textChanged, this,
                         [analysisTimer] {
                             analysisTimer->start();
                         });
        QObject::connect(this->caseSensitive_, &QCheckBox::toggled, this,
                         [this] {
                             this->updateSummary();
                         });
        QObject::connect(this->regex_, &QCheckBox::toggled, this, [this] {
            this->updateSummary();
        });
        QObject::connect(importRules, &QPushButton::clicked, this, [this] {
            const QPointer<WordListEditorDialog> self(this);
            auto imported =
                openWordListFile(this, "Import rules into moderation list");
            if (!self || !imported)
            {
                return;
            }
            if (imported->chattySyntax)
            {
                QMessageBox::information(
                    this, "Not a list of rules",
                    "This file holds the highlight settings of another "
                    "program, which can't be imported.");
                return;
            }
            if (imported->text.trimmed().isEmpty())
            {
                QMessageBox::warning(
                    this, "No rules found",
                    "The selected file does not contain any usable text.");
                return;
            }

            const auto currentText = this->terms_->toPlainText();
            bool append = false;
            if (!currentText.trimmed().isEmpty())
            {
                QPointer<QMessageBox> choice = new QMessageBox(
                    QMessageBox::Question, "Import rules",
                    "This list already contains rules. Replace them or add "
                    "the imported rules to the existing list?",
                    QMessageBox::Cancel, this);
                const auto cleanup = qScopeGuard([choice] {
                    delete choice;
                });
                auto *replaceButton =
                    choice->addButton("Replace rules", QMessageBox::AcceptRole);
                auto *appendButton = choice->addButton("Add to existing rules",
                                                       QMessageBox::ActionRole);
                choice->exec();
                if (!self || !choice)
                {
                    return;
                }
                if (choice->clickedButton() ==
                    choice->button(QMessageBox::Cancel))
                {
                    return;
                }
                append = choice->clickedButton() == appendButton;
                if (choice->clickedButton() != replaceButton && !append)
                {
                    return;
                }
            }

            if (append && imported->suggestedRegex != this->isRegex())
            {
                const auto message = imported->suggestedRegex
                                         ? "This file looks like a regular "
                                           "expression list, but this list "
                                           "uses words and phrases. The new "
                                           "rules would be treated as plain "
                                           "text. Add them anyway?"
                                         : "This file looks like a word and "
                                           "phrase list, but this list uses "
                                           "regular expressions. The new "
                                           "rules would be interpreted as "
                                           "expressions. Add them anyway?";
                if (QMessageBox::warning(this, "Different rule format", message,
                                         QMessageBox::Yes | QMessageBox::Cancel,
                                         QMessageBox::Cancel) !=
                    QMessageBox::Yes)
                {
                    return;
                }
            }

            if (!self)
            {
                return;
            }
            if (append)
            {
                auto combined = currentText;
                if (!combined.endsWith(u'\n'))
                {
                    combined.append(u'\n');
                }
                combined.append(imported->text);
                this->terms_->setPlainText(std::move(combined));
            }
            else
            {
                this->terms_->setPlainText(imported->text);
            }
            if (this->name_->text().trimmed().isEmpty())
            {
                this->name_->setText(imported->suggestedName);
            }
            if (!append && imported->suggestedRegex)
            {
                this->regex_->setChecked(true);
                this->caseSensitive_->setChecked(true);
            }
        });
        QObject::connect(this->customSoundButton_, &QPushButton::clicked, this,
                         [this] {
                             const QPointer<WordListEditorDialog> self(this);
                             const auto sound = QFileDialog::getOpenFileUrl(
                                 this, "Choose moderation list sound", QUrl(),
                                 "Audio files (*.mp3 *.wav)");
                             if (self && !sound.isEmpty())
                             {
                                 this->soundUrl_ = sound.toString();
                                 this->updateSoundButtons();
                             }
                         });
        QObject::connect(this->clearSoundButton_, &QPushButton::clicked, this,
                         [this] {
                             this->soundUrl_.clear();
                             this->updateSoundButtons();
                         });
        QObject::connect(
            this->colorButton_, &QPushButton::clicked, this, [this] {
                auto *picker = new ColorPickerDialog(this->color_, this);
                QObject::connect(picker, &ColorPickerDialog::colorConfirmed,
                                 this, [this](const QColor &color) {
                                     if (color.isValid())
                                     {
                                         this->color_ = color;
                                         this->updateColorButton();
                                     }
                                 });
                picker->show();
            });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            const QPointer<WordListEditorDialog> self(this);
            const auto parsed = parseWordListTerms(
                this->terms_->toPlainText(), this->caseSensitive_->isChecked(),
                this->isRegex());
            if (this->name_->text().trimmed().isEmpty())
            {
                QMessageBox::warning(this, "Missing name",
                                     "Give this word list a name.");
                return;
            }
            if (parsed.invalidRegex > 0)
            {
                QMessageBox::warning(
                    this, "Invalid regular expression",
                    QString("Fix the invalid expression before saving. %1")
                        .arg(parsed.firstRegexError));
                return;
            }
            if (parsed.overLimit > 0 || parsed.overCharacterBudget > 0 ||
                parsed.oversized > 0)
            {
                if (this->isRegex())
                {
                    QMessageBox::warning(
                        this, "Regular expression list is too large",
                        "Use up to 512 characters per expression, 2,000 rules, "
                        "and 500,000 characters total.");
                }
                else
                {
                    QMessageBox::warning(
                        this, "Word and phrase list is too large",
                        "Use up to 256 characters per entry, 10,000 rules, "
                        "and 500,000 characters total.");
                }
                return;
            }
            if (parsed.terms.empty())
            {
                QMessageBox::warning(
                    this, "No usable rules",
                    this->isRegex()
                        ? "Add at least one valid regular expression of 512 "
                          "characters or fewer."
                        : "Add at least one word or phrase of 256 characters "
                          "or fewer.");
                return;
            }
            if (this->isRegex() &&
                parsed.terms.size() > HIGHLIGHT_WORD_LIST_REGEX_WARNING_RULES &&
                QMessageBox::warning(
                    this, "Large regular expression list",
                    QString("This list has %1 regular expressions. Complex "
                            "rules may slow busy chats. Save anyway?")
                        .arg(parsed.terms.size()),
                    QMessageBox::Save | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Save)
            {
                return;
            }
            if (self)
            {
                this->accept();
            }
        });
    }

    HighlightWordList value() const
    {
        auto parsed = parseWordListTerms(this->terms_->toPlainText(),
                                         this->caseSensitive_->isChecked(),
                                         this->isRegex());
        return {
            this->name_->text().trimmed(),
            this->enabled_->isChecked(),
            std::move(parsed.terms),
            parseChannels(this->channels_->text()),
            this->caseSensitive_->isChecked(),
            this->showInMentions_->isChecked(),
            this->alert_->isChecked(),
            this->sound_->isChecked(),
            this->soundUrl_,
            this->color_,
            this->isRegex(),
            this->matchAppearance_->color(),
            this->matchAppearance_->style(),
            QString{},
            static_cast<HighlightWordListPlatform>(
                this->platform_->currentData().toInt()),
        };
    }

private:
    bool isRegex() const
    {
        return this->regex_->isChecked();
    }

    void updateColorButton()
    {
        this->colorButton_->setText(this->color_.name(QColor::HexArgb));
        QPixmap swatch(16, 16);
        swatch.fill(this->color_);
        this->colorButton_->setIcon(QIcon(swatch));
    }

    void updateSoundButtons()
    {
        const auto sound = QUrl(this->soundUrl_);
        this->customSoundButton_->setText(
            sound.isEmpty()
                ? "Custom sound: use highlight default"
                : "Custom sound: " + shortenString(sound.fileName(), 50));
        this->customSoundButton_->setToolTip(
            sound.isEmpty() ? QString() : sound.fileName());
        this->clearSoundButton_->setVisible(!sound.isEmpty());
    }

    void updateSummary()
    {
        const auto parsed = parseWordListTerms(
            this->terms_->toPlainText(), this->caseSensitive_->isChecked(),
            this->isRegex());
        if (this->isRegex())
        {
            this->intro_->setText("One regular expression per line.");
            this->terms_->setPlaceholderText(
                "\\b(?:badword|bad phrase)\\b\nspam\\d{2,}");
        }
        else
        {
            this->intro_->setText(
                "One word or phrase per line. A word will not match inside a "
                "longer word.");
            this->terms_->setPlaceholderText("bad phrase\nanother phrase");
        }
        QStringList ignored;
        if (parsed.blanks > 0)
        {
            ignored.append(QString::number(parsed.blanks) + " blank");
        }
        if (parsed.duplicates > 0)
        {
            ignored.append(QString::number(parsed.duplicates) + " duplicate");
        }
        if (parsed.oversized > 0)
        {
            ignored.append(QString::number(parsed.oversized) +
                           (this->isRegex() ? " over 512 characters"
                                            : " over 256 characters"));
        }
        if (parsed.overLimit > 0)
        {
            ignored.append(QString::number(parsed.overLimit) +
                           (this->isRegex() ? " past the 2,000 rule limit"
                                            : " past the 10,000 rule limit"));
        }
        if (parsed.overCharacterBudget > 0)
        {
            ignored.append(QString::number(parsed.overCharacterBudget) +
                           " past the 500,000 character list limit");
        }
        QString summary = QString("%1 usable %2")
                              .arg(parsed.terms.size())
                              .arg(parsed.terms.size() == 1 ? "rule" : "rules");
        if (!ignored.isEmpty())
        {
            summary += ". Ignored: " + ignored.join(", ") + ".";
        }
        if (parsed.invalidRegex > 0)
        {
            summary += QString(" Fix %1 invalid %2. %3")
                           .arg(parsed.invalidRegex)
                           .arg(parsed.invalidRegex == 1 ? "expression"
                                                         : "expressions")
                           .arg(parsed.firstRegexError);
        }
        if (this->isRegex() &&
            parsed.terms.size() > HIGHLIGHT_WORD_LIST_REGEX_WARNING_RULES &&
            parsed.overLimit == 0)
        {
            summary += " Large list: complex expressions may slow busy chats.";
        }
        this->summary_->setText(summary);
        this->updateSoundButtons();
        this->updateColorButton();
    }

    QLineEdit *name_{};
    QLineEdit *channels_{};
    QComboBox *platform_{};
    QCheckBox *regex_{};
    QLabel *intro_{};
    QPlainTextEdit *terms_{};
    QCheckBox *enabled_{};
    QCheckBox *caseSensitive_{};
    QCheckBox *showInMentions_{};
    QCheckBox *alert_{};
    QCheckBox *sound_{};
    QPushButton *customSoundButton_{};
    QPushButton *clearSoundButton_{};
    QPushButton *colorButton_{};
    QLabel *summary_{};
    MatchAppearanceControls *matchAppearance_{};
    QColor color_;
    QString soundUrl_;
};

}  // namespace

HighlightWordListsWidget::HighlightWordListsWidget()
{
    LayoutCreator<HighlightWordListsWidget> layoutCreator(this);
    auto wordLists = layoutCreator.emplace<QVBoxLayout>();
    {
        auto description = wordLists.emplace<QLabel>(
            "Highlight messages with reusable word, phrase, or regex "
            "lists. Limit a list to specific platforms or channels "
            "when needed.");
        description->setWordWrap(true);

        this->wordLists_ = wordLists.emplace<QTreeWidget>().getElement();
        this->wordLists_->setHeaderLabels({"Name", "List type", "Rules",
                                           "Platform", "Applies in",
                                           "Notifications"});
        this->wordLists_->setRootIsDecorated(false);
        this->wordLists_->setAlternatingRowColors(true);
        this->wordLists_->setUniformRowHeights(true);
        this->wordLists_->setSelectionMode(QAbstractItemView::SingleSelection);
        this->wordLists_->setSelectionBehavior(QAbstractItemView::SelectRows);
        this->wordLists_->header()->setSectionResizeMode(
            QHeaderView::ResizeToContents);
        this->wordLists_->header()->setSectionResizeMode(0,
                                                         QHeaderView::Stretch);
        QObject::connect(this->wordLists_, &QTreeWidget::itemDoubleClicked,
                         this, [this] {
                             this->editSelectedWordList();
                         });
        QObject::connect(
            this->wordLists_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *item, int column) {
                if (this->refreshingWordLists_ || column != 0)
                {
                    return;
                }
                const auto index = this->wordLists_->indexOfTopLevelItem(item);
                const auto lists = getSettings()->highlightWordLists.readOnly();
                if (index < 0 || index >= static_cast<int>(lists->size()))
                {
                    return;
                }
                auto updated = lists->at(index);
                updated.setEnabled(item->checkState(0) == Qt::Checked);
                getSettings()->highlightWordLists.removeAt(index);
                getSettings()->highlightWordLists.insert(updated, index);
                this->refreshWordLists();
            });

        auto buttons = wordLists.emplace<QHBoxLayout>().withoutMargin();
        auto add = buttons.emplace<QPushButton>("New list");
        auto import = buttons.emplace<QPushButton>("Import");
        auto edit = buttons.emplace<QPushButton>("Edit list");
        auto exportButton = buttons.emplace<QPushButton>("Export list");
        auto remove = buttons.emplace<QPushButton>("Remove list");
        for (auto *button :
             {add.getElement(), import.getElement(), edit.getElement(),
              exportButton.getElement(), remove.getElement()})
        {
            button->setMinimumWidth(std::max(96, button->sizeHint().width()));
        }
        buttons->addStretch(1);
        const auto updateSelectionButtons =
            [edit = edit.getElement(), exportButton = exportButton.getElement(),
             remove = remove.getElement()](QTreeWidgetItem *current,
                                           QTreeWidgetItem *) {
                const auto hasSelection = current != nullptr;
                edit->setEnabled(hasSelection);
                exportButton->setEnabled(hasSelection);
                remove->setEnabled(hasSelection);
            };
        QObject::connect(add.getElement(), &QPushButton::clicked, this, [this] {
            this->createWordList();
        });
        QObject::connect(import.getElement(), &QPushButton::clicked, this,
                         [this] {
                             this->importWordList();
                         });
        QObject::connect(edit.getElement(), &QPushButton::clicked, this,
                         [this] {
                             this->editSelectedWordList();
                         });
        QObject::connect(exportButton.getElement(), &QPushButton::clicked, this,
                         [this] {
                             this->exportSelectedWordList();
                         });
        QObject::connect(remove.getElement(), &QPushButton::clicked, this,
                         [this] {
                             this->removeSelectedWordList();
                         });
        QObject::connect(this->wordLists_, &QTreeWidget::currentItemChanged,
                         this, updateSelectionButtons);
        updateSelectionButtons(nullptr, nullptr);
    }

    this->refreshWordLists();
}

int HighlightWordListsWidget::selectedWordListIndex() const
{
    return this->wordLists_ ? this->wordLists_->indexOfTopLevelItem(
                                  this->wordLists_->currentItem())
                            : -1;
}

void HighlightWordListsWidget::refreshWordLists()
{
    if (!this->wordLists_)
    {
        return;
    }
    const auto previous = this->selectedWordListIndex();
    this->refreshingWordLists_ = true;
    QSignalBlocker blocker(this->wordLists_);
    this->wordLists_->clear();
    const auto lists = getSettings()->highlightWordLists.readOnly();
    for (const auto &list : *lists)
    {
        QStringList notifications;
        if (list.showInMentions())
        {
            notifications.append("Mentions");
        }
        if (list.hasAlert())
        {
            notifications.append("Taskbar");
        }
        if (list.hasSound())
        {
            notifications.append("Sound");
        }

        auto *item = new QTreeWidgetItem(
            {list.name(),
             list.isRegex() ? "Regular expressions" : "Words and phrases",
             QString::number(list.terms().size()),
             highlightWordListPlatformName(list.platform()),
             list.channels().empty() ? QStringLiteral("Everywhere")
                                     : QString::number(list.channels().size()),
             notifications.isEmpty() ? QStringLiteral("Highlight only")
                                     : notifications.join(", ")});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, list.enabled() ? Qt::Checked : Qt::Unchecked);
        QPixmap swatch(16, 16);
        swatch.fill(*list.color());
        item->setIcon(0, QIcon(swatch));
        const auto appearance = highlightMatchStyleName(list.matchStyle());
        item->setToolTip(
            0, QString("Message color %1\nExact marker %2")
                   .arg(list.color()->name(QColor::HexArgb), appearance));
        item->setToolTip(
            1, list.caseSensitive() ? "Case sensitive" : "Case insensitive");
        if (list.channels().empty())
        {
            item->setToolTip(4, "Applies in all channels");
        }
        else
        {
            QStringList channels;
            for (const auto &channel : list.channels())
            {
                channels.append(channel);
            }
            item->setToolTip(
                4,
                QString("Applies in %1 selected %2:\n%3")
                    .arg(list.channels().size())
                    .arg(list.channels().size() == 1 ? "channel" : "channels")
                    .arg(channels.join(", ")));
        }
        this->wordLists_->addTopLevelItem(item);
    }
    blocker.unblock();
    this->refreshingWordLists_ = false;
    if (this->wordLists_->topLevelItemCount() > 0)
    {
        this->wordLists_->setCurrentItem(
            this->wordLists_->topLevelItem(std::clamp(
                previous, 0, this->wordLists_->topLevelItemCount() - 1)));
    }
}

void HighlightWordListsWidget::createWordList()
{
    QPointer<WordListEditorDialog> dialog =
        new WordListEditorDialog(this, std::nullopt);
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted)
    {
        return;
    }
    getSettings()->highlightWordLists.append(dialog->value());
    this->refreshWordLists();
    this->wordLists_->setCurrentItem(this->wordLists_->topLevelItem(
        this->wordLists_->topLevelItemCount() - 1));
}

void HighlightWordListsWidget::importWordList()
{
    const QPointer<HighlightWordListsWidget> self(this);
    auto imported = openWordListFile(this, "Import moderation list");
    if (!self || !imported)
    {
        return;
    }
    if (imported->chattySyntax)
    {
        QMessageBox::information(
            this, "Not a list of rules",
            "This file holds the highlight settings of another program, "
            "which can't be imported.");
        return;
    }
    QPointer<WordListEditorDialog> dialog = new WordListEditorDialog(
        this, std::nullopt, imported->text, imported->suggestedName,
        imported->suggestedRegex);
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted)
    {
        return;
    }
    getSettings()->highlightWordLists.append(dialog->value());
    this->refreshWordLists();
    this->wordLists_->setCurrentItem(this->wordLists_->topLevelItem(
        this->wordLists_->topLevelItemCount() - 1));
}

void HighlightWordListsWidget::editSelectedWordList()
{
    const auto index = this->selectedWordListIndex();
    const auto lists = getSettings()->highlightWordLists.readOnly();
    if (index < 0 || index >= static_cast<int>(lists->size()))
    {
        return;
    }

    QPointer<WordListEditorDialog> dialog =
        new WordListEditorDialog(this, lists->at(index));
    const auto cleanup = qScopeGuard([dialog] {
        delete dialog;
    });
    const auto result = dialog->exec();
    if (!dialog || result != QDialog::Accepted ||
        getSettings()->highlightWordLists.readOnly() != lists)
    {
        return;
    }
    getSettings()->highlightWordLists.removeAt(index);
    getSettings()->highlightWordLists.insert(dialog->value(), index);
    this->refreshWordLists();
    this->wordLists_->setCurrentItem(this->wordLists_->topLevelItem(index));
}

void HighlightWordListsWidget::exportSelectedWordList()
{
    const auto index = this->selectedWordListIndex();
    const auto lists = getSettings()->highlightWordLists.readOnly();
    if (index < 0 || index >= static_cast<int>(lists->size()))
    {
        return;
    }
    const auto &list = lists->at(index);
    const QPointer<HighlightWordListsWidget> self(this);
    const auto fileName = QFileDialog::getSaveFileName(
        this, "Export moderation list", list.name() + ".txt",
        "Text files (*.txt)");
    if (!self || fileName.isEmpty())
    {
        return;
    }

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(
            this, "Could not export list",
            "JilChat Chatterino could not write the selected file.");
        return;
    }
    QStringList lines;
    for (const auto &term : list.terms())
    {
        lines.append(term);
    }
    file.write(lines.join(u'\n').toUtf8());
    file.write("\n");
}

void HighlightWordListsWidget::removeSelectedWordList()
{
    const auto index = this->selectedWordListIndex();
    const auto lists = getSettings()->highlightWordLists.readOnly();
    if (index < 0 || index >= static_cast<int>(lists->size()))
    {
        return;
    }
    const QPointer<HighlightWordListsWidget> self(this);
    const auto ruleCount = lists->at(index).terms().size();
    QPointer<QMessageBox> confirmation = new QMessageBox(
        QMessageBox::Question, "Remove moderation list",
        QString(ruleCount == 1 ? "Remove \"%1\" and its %2 rule?"
                               : "Remove \"%1\" and its %2 rules?")
            .arg(lists->at(index).name())
            .arg(ruleCount),
        QMessageBox::Yes | QMessageBox::No, this);
    const auto cleanup = qScopeGuard([confirmation] {
        delete confirmation;
    });
    confirmation->button(QMessageBox::Yes)->setText("Remove list");
    confirmation->button(QMessageBox::No)->setText("Cancel");
    const auto result = confirmation->exec();
    if (!self || !confirmation || result != QMessageBox::Yes ||
        getSettings()->highlightWordLists.readOnly() != lists)
    {
        return;
    }
    getSettings()->highlightWordLists.removeAt(index);
    this->refreshWordLists();
}

}  // namespace chatterino
