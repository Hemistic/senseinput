#include "audio_io.h"
#include "fsmn_vad_engine.h"
#include "sensevoice_engine.h"
#include "stream_recognizer.h"
#include "text_processor.h"
#include "windows_text_injector.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QAbstractTextDocumentLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadialGradient>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct AccentPolicy {
    int state;
    int flags;
    DWORD gradient_color;
    int animation_id;
};

struct WindowCompositionAttributeData {
    int attribute;
    void* data;
    SIZE_T size;
};

using SetWindowCompositionAttributeFn = BOOL (WINAPI *)(HWND, WindowCompositionAttributeData*);

struct DwmBlurBehind {
    DWORD flags;
    BOOL enable;
    HRGN blur_region;
    BOOL transition_on_maximized;
};

using DwmEnableBlurBehindWindowFn = HRESULT (WINAPI *)(HWND, const DwmBlurBehind*);
#endif

namespace {

constexpr int bubble_minimum_width = 132;
constexpr int bubble_maximum_size = 520;
constexpr int bubble_maximum_width = bubble_maximum_size;
constexpr int bubble_maximum_height = bubble_maximum_size;
constexpr int bubble_minimum_height = 32;
constexpr int bubble_horizontal_padding = 32;
constexpr int bubble_vertical_padding = 24;
constexpr int window_margin = 6;
constexpr int control_spacing = 8;

enum class BubbleStyle {
    Capsule,
    Panel,
    Ring,
};

QString bubbleStyleName(BubbleStyle style) {
    switch (style) {
    case BubbleStyle::Capsule:
        return QStringLiteral("capsule");
    case BubbleStyle::Panel:
        return QStringLiteral("panel");
    case BubbleStyle::Ring:
        return QStringLiteral("ring");
    }
    return QStringLiteral("panel");
}

BubbleStyle bubbleStyleFromName(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("capsule") || normalized == QStringLiteral("pill")) {
        return BubbleStyle::Capsule;
    }
    if (normalized == QStringLiteral("ring") || normalized == QStringLiteral("circle")) {
        return BubbleStyle::Ring;
    }
    return BubbleStyle::Panel;
}

QString to_qstring(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string to_utf8_string(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

struct VadSettings {
    int endpoint_ms = 700;
    int threshold_percent = 55;
    int minimum_db = -60;
    int snr_db = 3;
};

enum class TextMode {
    Raw,
    Clean,
};

constexpr auto default_hotkey = "Ctrl+Alt+Space";
constexpr auto control_windows_hotkey = "Ctrl+Win";

QString canonicalShortcut(const QString& value) {
    QString compact = value;
    compact.remove(QLatin1Char(' '));
    if (compact.compare(QString::fromLatin1(control_windows_hotkey), Qt::CaseInsensitive) == 0) {
        return QString::fromLatin1(control_windows_hotkey);
    }

    const QKeySequence sequence = QKeySequence::fromString(value, QKeySequence::PortableText);
    if (sequence.isEmpty() || sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown) {
        return {};
    }
    return sequence.toString(QKeySequence::PortableText);
}

bool hasUsableShortcutKey(const QString& shortcut) {
    if (shortcut == QString::fromLatin1(control_windows_hotkey)) return true;

    const QKeySequence sequence = QKeySequence::fromString(shortcut, QKeySequence::PortableText);
    if (sequence.isEmpty() || sequence.count() != 1) return false;
    const Qt::Key key = sequence[0].key();
    return key != Qt::Key_unknown && key != Qt::Key_Control && key != Qt::Key_Shift &&
        key != Qt::Key_Alt && key != Qt::Key_Meta;
}

QString shortcutDisplayName(const QString& shortcut) {
    if (shortcut == QString::fromLatin1(control_windows_hotkey)) {
        return QStringLiteral("Ctrl + Win");
    }
    const QKeySequence sequence = QKeySequence::fromString(shortcut, QKeySequence::PortableText);
    const QString native_text = sequence.toString(QKeySequence::NativeText);
    return native_text.isEmpty() ? shortcut : native_text;
}

QIcon sensevoiceIcon() {
    static const QIcon icon(QStringLiteral(":/icons/sensevoice.ico"));
    return icon;
}

class LevelWaveform final : public QWidget {
public:
    explicit LevelWaveform(QWidget* parent = nullptr)
        : QWidget(parent), ios9_animation_timer_(this) {
        setFixedSize(88, 24);
        ios9_animation_timer_.setInterval(16);
        ios9_animation_timer_.setTimerType(Qt::PreciseTimer);
        connect(&ios9_animation_timer_, &QTimer::timeout, this, [this] {
            advanceIos9Animation();
        });
    }

    void setCircular(bool circular) {
        circular_ = circular;
        if (circular_) {
            strip_ = false;
            setFixedSize(54, 54);
        } else if (!strip_) {
            setFixedSize(88, 24);
        }
        update();
    }

    void setStrip(bool strip) {
        strip_ = strip;
        if (strip_) {
            circular_ = false;
            setFixedSize(98, 24);
        } else if (circular_) {
            setFixedSize(54, 54);
        } else {
            setFixedSize(88, 24);
        }
        update();
    }

    void setTelemetry(float input_db, VadActivity activity) {
        input_db_ = input_db;
        activity_ = activity;
        if (strip_) {
            const float normalized = std::clamp((input_db_ + 60.0F) / 48.0F, 0.0F, 1.0F);
            ios9_target_amplitude_ = active_
                ? std::clamp((normalized - 0.08F) / 0.48F, 0.0F, 1.0F)
                : 0.0F;
        } else {
            phase_ += 0.32F;
        }
        update();
    }

    void setActive(bool active) {
        const bool was_active = active_;
        active_ = active;
        if (strip_) {
            if (active_) {
                if (!was_active) {
                    resetIos9Animation();
                    ios9_clock_.restart();
                }
                ios9_animation_timer_.start();
            } else {
                ios9_animation_timer_.stop();
                ios9_target_amplitude_ = 0.0F;
                ios9_amplitude_ = 0.0F;
            }
        }
        update();
    }

    void setPreviewSignal() {
        setActive(true);
        ios9_target_amplitude_ = 1.0F;
        for (int frame = 0; frame < 42; ++frame) advanceIos9Animation();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const float normalized_level = active_
            ? std::clamp((input_db_ + 60.0F) / 48.0F, 0.0F, 1.0F)
            : 0.0F;
        QColor color(116, 121, 130);
        if (activity_ == VadActivity::Candidate) color = QColor(232, 128, 104);
        else if (activity_ == VadActivity::Speech) color = QColor(243, 184, 75);
        else if (activity_ == VadActivity::EndpointWait) color = QColor(103, 193, 170);
        else if (circular_) color = QColor(78, 211, 178);

        if (circular_) {
            const QPointF center(width() / 2.0, height() / 2.0);
            const qreal radius = std::min(width(), height()) / 2.0 - 2.0;
            const qreal wave_radius = radius - 5.0;
            const float level = active_
                ? 0.55F + normalized_level * 0.85F
                : 0.50F + 0.06F * std::sin(phase_ * 0.35F);

            // SiriWave-style construction: several attenuated sine curves with
            // independent phase/frequency, clipped to a compact circular orb.
            QRadialGradient glow(center, radius * 0.95);
            const int glow_alpha = active_ ? 34 + static_cast<int>(normalized_level * 34.0F) : 18;
            glow.setColorAt(0.0, QColor(color.red(), color.green(), color.blue(), glow_alpha));
            glow.setColorAt(0.55, QColor(color.red(), color.green(), color.blue(), glow_alpha / 3));
            glow.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
            painter.setPen(Qt::NoPen);
            painter.setBrush(glow);
            painter.drawEllipse(center, radius, radius);

            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(78, 211, 178, active_ ? 190 : 120),
                                active_ ? 1.4 : 1.1));
            painter.drawEllipse(center, radius - 3.0, radius - 3.0);

            QPainterPath clip_path;
            clip_path.addEllipse(center, wave_radius, wave_radius);
            painter.save();
            painter.setClipPath(clip_path);

            const std::array<float, 5> amplitudes = {0.66F, 0.88F, 1.0F, 0.84F, 0.62F};
            const std::array<float, 5> frequencies = {2.1F, 2.55F, 2.9F, 2.45F, 2.0F};
            const std::array<float, 5> phase_offsets = {0.25F, 1.45F, 2.4F, 3.35F, 4.7F};
            const std::array<QColor, 5> wave_colors = {
                QColor(87, 155, 255),
                QColor(70, 218, 224),
                QColor(92, 238, 173),
                QColor(174, 135, 255),
                QColor(255, 126, 176),
            };
            constexpr int sample_count = 44;
            const qreal vertical_scale = wave_radius * 0.72 * level;
            for (std::size_t curve = 0; curve < amplitudes.size(); ++curve) {
                QPainterPath path;
                for (int sample = 0; sample <= sample_count; ++sample) {
                    const qreal x = -1.0 + 2.0 * sample / sample_count;
                    const qreal attenuation = std::pow(std::max(0.0, 1.0 - x * x), 1.15);
                    const qreal wave = std::sin(
                        frequencies[curve] * M_PI * x + phase_ * (0.72 + curve * 0.08) +
                        phase_offsets[curve]);
                    const qreal y = wave * attenuation * vertical_scale * amplitudes[curve];
                    const QPointF point(center.x() + x * wave_radius,
                                        center.y() + y);
                    if (sample == 0) path.moveTo(point);
                    else path.lineTo(point);
                }

                const QColor wave_color = wave_colors[curve];
                painter.setPen(QPen(QColor(wave_color.red(), wave_color.green(), wave_color.blue(),
                                           active_ ? 52 : 34),
                                    active_ ? 4.0 : 3.2,
                                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawPath(path);
                painter.setPen(QPen(QColor(wave_color.red(), wave_color.green(), wave_color.blue(),
                                           active_ ? 218 : 172),
                                    active_ ? 1.5 : 1.2,
                                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawPath(path);
            }
            painter.restore();

            QRadialGradient core(center, active_ ? 6.5 : 4.8);
            core.setColorAt(0.0, QColor(239, 255, 250, active_ ? 245 : 220));
            core.setColorAt(0.35, QColor(111, 239, 198, active_ ? 180 : 130));
            core.setColorAt(1.0, QColor(78, 211, 178, 0));
            painter.setPen(Qt::NoPen);
            painter.setBrush(core);
            painter.drawEllipse(center, active_ ? 6.5 : 4.8, active_ ? 6.5 : 4.8);
            painter.setBrush(QColor(241, 255, 251, active_ ? 245 : 215));
            painter.drawEllipse(center, active_ ? 2.5 : 2.2, active_ ? 2.5 : 2.2);
            return;
        }

        if (strip_) {
            drawIos9Waveform(painter);
            return;
        }

        painter.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap));
        constexpr int bars = 16;
        constexpr qreal gap = 5.4;
        const qreal first_x = (width() - (bars - 1) * gap) / 2.0;
        for (int index = 0; index < bars; ++index) {
            const float wave = 0.35F + 0.65F * std::abs(std::sin(phase_ + index * 0.82F));
            const qreal line_height = active_ ? 2.0 + normalized_level * (4.0 + wave * 13.0) : 2.0;
            const qreal x = first_x + index * gap;
            painter.drawLine(QPointF(x, height() / 2.0 - line_height / 2.0),
                             QPointF(x, height() / 2.0 + line_height / 2.0));
        }
    }

private:
    struct Ios9CurveState {
        int no_of_curves = 0;
        qint64 spawn_at_ms = 0;
        float previous_max_y = 0.0F;
        std::array<float, 5> phases{};
        std::array<float, 5> amplitudes{};
        std::array<float, 5> despawn_timeouts{};
        std::array<float, 5> offsets{};
        std::array<float, 5> speeds{};
        std::array<float, 5> final_amplitudes{};
        std::array<float, 5> widths{};
        std::array<float, 5> verses{};
    };

    float randomRange(float minimum, float maximum) {
        std::uniform_real_distribution<float> distribution(minimum, maximum);
        return distribution(random_engine_);
    }

    static qreal ios9GlobalAttenuation(qreal x) {
        constexpr qreal attack_factor = 4.0;
        return std::pow(attack_factor / (attack_factor + std::pow(x, 2.0)), attack_factor);
    }

    void spawnIos9Layer(Ios9CurveState& layer) {
        layer = Ios9CurveState{};
        layer.spawn_at_ms = ios9_clock_.elapsed();
        layer.no_of_curves = static_cast<int>(std::floor(randomRange(2.0F, 5.0F)));
        for (int index = 0; index < layer.no_of_curves; ++index) {
            layer.despawn_timeouts[index] = randomRange(500.0F, 2000.0F);
            layer.offsets[index] = randomRange(-3.0F, 3.0F);
            layer.speeds[index] = randomRange(0.5F, 1.0F);
            layer.final_amplitudes[index] = randomRange(0.3F, 1.0F);
            layer.widths[index] = randomRange(1.0F, 3.0F);
            layer.verses[index] = randomRange(-1.0F, 1.0F);
        }
    }

    void resetIos9Animation() {
        ios9_amplitude_ = 0.0F;
        ios9_target_amplitude_ = 0.0F;
        for (Ios9CurveState& layer : ios9_layers_) layer = Ios9CurveState{};
    }

    void advanceIos9Animation() {
        if (!strip_ || !active_) return;

        // This is SiriWave's lerpSpeed (0.1) and speed (0.2), sampled at
        // roughly the same 60 Hz cadence as the original canvas animation.
        ios9_amplitude_ += (ios9_target_amplitude_ - ios9_amplitude_) * 0.1F;
        const qint64 now_ms = ios9_clock_.elapsed();
        for (Ios9CurveState& layer : ios9_layers_) {
            if (layer.no_of_curves == 0) spawnIos9Layer(layer);
            for (int index = 0; index < layer.no_of_curves; ++index) {
                if (layer.spawn_at_ms + layer.despawn_timeouts[index] <= now_ms) {
                    layer.amplitudes[index] -= 0.02F;
                } else {
                    layer.amplitudes[index] += 0.02F;
                }
                layer.amplitudes[index] = std::clamp(
                    layer.amplitudes[index], 0.0F, layer.final_amplitudes[index]);
                layer.phases[index] = std::fmod(
                    layer.phases[index] + 0.2F * layer.speeds[index],
                    static_cast<float>(2.0 * M_PI));
            }
        }
        update();
    }

    qreal ios9RelativePosition(const Ios9CurveState& layer, qreal i) const {
        qreal y = 0.0;
        for (int index = 0; index < layer.no_of_curves; ++index) {
            const qreal t = 4.0 * (-1.0 +
                (static_cast<qreal>(index) / (layer.no_of_curves - 1)) * 2.0) +
                layer.offsets[index];
            const qreal x = i / layer.widths[index] - t;
            y += std::abs(layer.amplitudes[index] *
                std::sin(layer.verses[index] * x - layer.phases[index]) *
                ios9GlobalAttenuation(x));
        }
        return layer.no_of_curves == 0 ? 0.0 : y / layer.no_of_curves;
    }

    void drawIos9Waveform(QPainter& painter) {
        constexpr qreal graph_x = 25.0;
        constexpr qreal amplitude_factor = 3.8;
        const qreal height_max = height() / 2.0;
        const qreal baseline = height_max;

        QLinearGradient support_line(0.0, 0.0, width(), 0.0);
        support_line.setColorAt(0.0, QColor(255, 255, 255, 0));
        support_line.setColorAt(0.1, QColor(255, 255, 255, 128));
        support_line.setColorAt(0.8, QColor(255, 255, 255, 128));
        support_line.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(support_line);
        painter.drawRect(QRectF(0.0, baseline, width(), 1.0));

        if (!active_ || ios9_amplitude_ <= 0.001F) return;

        static constexpr std::array<QColor, 3> colors = {
            QColor(15, 82, 169),
            QColor(173, 57, 76),
            QColor(48, 220, 155),
        };
        painter.save();
        painter.setOpacity(0.7);
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        for (std::size_t layer_index = 0; layer_index < colors.size(); ++layer_index) {
            Ios9CurveState& layer = ios9_layers_[layer_index];
            if (layer.no_of_curves == 0) spawnIos9Layer(layer);
            qreal maximum_y = -std::numeric_limits<qreal>::infinity();
            for (const qreal sign : {1.0, -1.0}) {
                QPainterPath path;
                bool first_point = true;
                for (qreal i = -graph_x; i <= graph_x; i += 0.02) {
                    const qreal x = width() * ((i + graph_x) / (graph_x * 2.0));
                    const qreal raw_y = amplitude_factor * height_max * ios9_amplitude_ *
                        ios9RelativePosition(layer, i) *
                        ios9GlobalAttenuation((i / graph_x) * 2.0);
                    const qreal y = std::clamp(raw_y, 0.0, std::max(0.0, height_max - 1.0));
                    const QPointF point(x, baseline - sign * y);
                    if (first_point) {
                        path.moveTo(point);
                        first_point = false;
                    } else {
                        path.lineTo(point);
                    }
                    maximum_y = std::max(maximum_y, y);
                }
                path.closeSubpath();
                painter.setBrush(colors[layer_index]);
                painter.setPen(QPen(colors[layer_index], 1.0));
                painter.drawPath(path);
            }
            if (maximum_y < 2.0 && layer.previous_max_y > maximum_y) {
                layer = Ios9CurveState{};
            }
            layer.previous_max_y = static_cast<float>(maximum_y);
        }
        painter.restore();
    }

    float input_db_ = -100.0F;
    float phase_ = 0.0F;
    VadActivity activity_ = VadActivity::Silence;
    bool active_ = false;
    bool circular_ = false;
    bool strip_ = false;
    QTimer ios9_animation_timer_;
    QElapsedTimer ios9_clock_;
    std::mt19937 random_engine_{std::random_device{}()};
    std::array<Ios9CurveState, 3> ios9_layers_{};
    float ios9_amplitude_ = 0.0F;
    float ios9_target_amplitude_ = 0.0F;
};

class VadStatusDot final : public QWidget {
public:
    explicit VadStatusDot(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(8, 8);
        setToolTip(QStringLiteral("VAD 静音"));
    }

    void setActivity(VadActivity activity) {
        activity_ = activity;
        switch (activity_) {
        case VadActivity::Speech:
            setToolTip(QStringLiteral("VAD 已触发：语音"));
            break;
        case VadActivity::EndpointWait:
            setToolTip(QStringLiteral("VAD 已触发：等待句尾"));
            break;
        case VadActivity::Candidate:
            setToolTip(QStringLiteral("VAD 候选"));
            break;
        case VadActivity::Silence:
            setToolTip(QStringLiteral("VAD 静音"));
            break;
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QColor color(220, 82, 92);
        if (activity_ == VadActivity::Candidate) color = QColor(242, 183, 74);
        else if (activity_ == VadActivity::Speech || activity_ == VadActivity::EndpointWait) {
            color = QColor(78, 220, 157);
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF center(width() / 2.0, height() / 2.0);
        QRadialGradient glow(center, 4.0);
        glow.setColorAt(0.0, QColor(color.red(), color.green(), color.blue(), 155));
        glow.setColorAt(0.65, QColor(color.red(), color.green(), color.blue(), 45));
        glow.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawEllipse(center, 4.0, 4.0);
        painter.setBrush(color);
        painter.drawEllipse(center, 2.5, 2.5);
    }

private:
    VadActivity activity_ = VadActivity::Silence;
};

class RecordingControl final : public QWidget {
public:
    explicit RecordingControl(BubbleStyle style = BubbleStyle::Ring, QWidget* parent = nullptr)
        : QWidget(parent), style_(style) {
        setObjectName(QStringLiteral("recordingControl"));
        setAttribute(Qt::WA_StyledBackground, true);
        layout_ = new QHBoxLayout(this);
        layout_->setContentsMargins(8, 7, 8, 7);
        layout_->setSpacing(8);

        cancel_button_ = new QToolButton;
        cancel_button_->setIcon(QWidget::style()->standardIcon(QStyle::SP_DialogCancelButton));
        cancel_button_->setToolTip(QStringLiteral("取消"));
        vad_dot_ = new VadStatusDot;
        waveform_ = new LevelWaveform;
        elapsed_label_ = new QLabel(QStringLiteral("00:00"));
        elapsed_label_->setFixedWidth(42);
        elapsed_label_->setAlignment(Qt::AlignCenter);
        elapsed_label_->setStyleSheet(QStringLiteral(
            "color: #D6D8DC; font-family: Consolas; font-size: 11px;"));
        mode_label_ = new QLabel(QStringLiteral("精简"));
        mode_label_->setObjectName(QStringLiteral("mode"));
        mode_label_->setFixedWidth(38);
        mode_label_->setAlignment(Qt::AlignCenter);
        mode_label_->setStyleSheet(QStringLiteral("color: #969BA4; font-size: 10px;"));
        primary_button_ = new QToolButton;
        primary_button_->setObjectName(QStringLiteral("primary"));
        primary_button_->setToolTip(QStringLiteral("开始"));

        layout_->addWidget(vad_dot_);
        layout_->addWidget(cancel_button_);
        layout_->addWidget(waveform_);
        layout_->addWidget(elapsed_label_);
        layout_->addWidget(mode_label_);
        layout_->addStretch();
        layout_->addWidget(primary_button_);

        connect(cancel_button_, &QToolButton::clicked, this, [this] {
            if (cancel_handler_) cancel_handler_();
        });
        connect(primary_button_, &QToolButton::clicked, this, [this] {
            if (primary_handler_) primary_handler_();
        });
        setVisualStyle(style_);
        setListening(false);
    }

    void setVisualStyle(BubbleStyle style) {
        style_ = style;
        const int width = style == BubbleStyle::Capsule ? 304 :
            (style == BubbleStyle::Ring ? 132 : 286);
        const int height = style == BubbleStyle::Ring ? 32 :
            (style == BubbleStyle::Capsule ? 54 : 48);
        setFixedSize(width, height);
        if (style == BubbleStyle::Ring) {
            layout_->setContentsMargins(10, 4, 10, 4);
            layout_->setSpacing(5);
        } else {
            layout_->setContentsMargins(8, 7, 8, 7);
            layout_->setSpacing(8);
        }
        waveform_->setCircular(false);
        waveform_->setStrip(style == BubbleStyle::Ring);
        if (style != BubbleStyle::Ring) waveform_->setFixedSize(88, 24);
        cancel_button_->setVisible(style != BubbleStyle::Ring);
        elapsed_label_->setVisible(style != BubbleStyle::Ring);
        primary_button_->setVisible(style != BubbleStyle::Ring);
        mode_label_->setVisible(style != BubbleStyle::Ring);
        setStyleSheet(styleSheetFor(style));
    }

    void setHandlers(std::function<void()> primary, std::function<void()> cancel) {
        primary_handler_ = std::move(primary);
        cancel_handler_ = std::move(cancel);
    }

    void setListening(bool listening) {
        listening_ = listening;
        waveform_->setActive(listening);
        cancel_button_->setEnabled(listening);
        primary_button_->setEnabled(true);
        primary_button_->setIcon(QWidget::style()->standardIcon(
            listening ? QStyle::SP_DialogApplyButton : QStyle::SP_MediaPlay));
        primary_button_->setToolTip(listening ? QStringLiteral("完成") : QStringLiteral("开始"));
        elapsed_label_->setText(listening ? QStringLiteral("00:00") : QStringLiteral("就绪"));
    }

    void setStopping(bool stopping) {
        waveform_->setActive(!stopping && listening_);
        cancel_button_->setEnabled(!stopping && listening_);
        primary_button_->setEnabled(!stopping);
        if (stopping) elapsed_label_->setText(QStringLiteral("处理中"));
    }

    void setTelemetry(float input_db, VadActivity activity) {
        waveform_->setTelemetry(input_db, activity);
        vad_dot_->setActivity(activity);
    }

    void setPreviewSignal() {
        setListening(true);
        waveform_->setPreviewSignal();
        waveform_->setTelemetry(-30.0F, VadActivity::Speech);
        vad_dot_->setActivity(VadActivity::Speech);
    }

    void setElapsedMilliseconds(qint64 milliseconds) {
        if (!listening_) return;
        const qint64 seconds = std::max<qint64>(0, milliseconds / 1000);
        elapsed_label_->setText(QStringLiteral("%1:%2")
                                    .arg(seconds / 60, 2, 10, QLatin1Char('0'))
                                    .arg(seconds % 60, 2, 10, QLatin1Char('0')));
    }

    void setMode(TextMode mode) {
        mode_label_->setText(mode == TextMode::Raw ? QStringLiteral("原文") : QStringLiteral("精简"));
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        if (style_ == BubbleStyle::Ring) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const QRectF control_rect = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
            painter.setPen(QPen(QColor(235, 236, 240, 78), 1.0));
            painter.setBrush(QColor(78, 80, 86, 72));
            painter.drawRoundedRect(control_rect, height() / 2.0, height() / 2.0);
        }
        QWidget::paintEvent(event);
    }

private:
    static QString styleSheetFor(BubbleStyle style) {
        switch (style) {
        case BubbleStyle::Capsule:
            return QStringLiteral(
                "QWidget#recordingControl { background: #14252B; border: 1px solid #31545A; border-radius: 27px; }"
                "QToolButton { width: 30px; height: 30px; border: none; border-radius: 15px; background: #213A40; color: #F1FCF8; }"
                "QToolButton:hover { background: #2D4B50; }"
                "QToolButton:disabled { background: #1C3035; color: #6E8D8D; }"
                "QToolButton#primary { background: #63D9B5; color: #0C2927; }"
                "QToolButton#primary:hover { background: #81E7C7; }"
                "QLabel { border: none; background: transparent; color: #D7F2EA; }"
                "QLabel#mode { color: #84B4AC; }");
        case BubbleStyle::Ring:
            return QStringLiteral(
                "QWidget#recordingControl { background: transparent; border: none; }"
                "QToolButton { width: 24px; height: 24px; border: none; border-radius: 12px; background: #5C5D63; color: #F2F2F7; }"
                "QToolButton:hover { background: #6B6C72; }"
                "QToolButton:disabled { background: #505158; color: #A5A6AC; }"
                "QToolButton#primary { background: #D1D1D6; color: #1C1C1E; }"
                "QToolButton#primary:hover { background: #E5E5EA; }"
                "QLabel { border: none; background: transparent; color: #F2F2F7; }");
        case BubbleStyle::Panel:
            return QStringLiteral(
                "QWidget#recordingControl { background: #20252C; border: 1px solid #3A424B; border-radius: 10px; }"
                "QToolButton { width: 30px; height: 30px; border: none; border-radius: 7px; background: #2D333B; color: #F5F6F7; }"
                "QToolButton:hover { background: #3A424C; }"
                "QToolButton:disabled { background: #252A30; color: #737B85; }"
                "QToolButton#primary { background: #F1F4F6; color: #1D2329; }"
                "QToolButton#primary:hover { background: #FFFFFF; }"
                "QLabel { border: none; background: transparent; color: #D6DBE1; }");
        }
        return {};
    }

    QHBoxLayout* layout_ = nullptr;
    QToolButton* cancel_button_ = nullptr;
    QToolButton* primary_button_ = nullptr;
    VadStatusDot* vad_dot_ = nullptr;
    LevelWaveform* waveform_ = nullptr;
    QLabel* elapsed_label_ = nullptr;
    QLabel* mode_label_ = nullptr;
    std::function<void()> primary_handler_;
    std::function<void()> cancel_handler_;
    BubbleStyle style_ = BubbleStyle::Ring;
    bool listening_ = false;
};

class TranscriptBubble final : public QWidget {
public:
    explicit TranscriptBubble(BubbleStyle style = BubbleStyle::Ring, QWidget* parent = nullptr)
        : QWidget(parent), style_(style) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        QFont text_font = font();
        text_font.setPointSize(10);
        setFont(text_font);
        base_font_ = text_font;
        document_.setDocumentMargin(0);
        document_.setDefaultFont(text_font);
        QTextOption text_option;
        text_option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document_.setDefaultTextOption(text_option);
    }

    void setStyle(BubbleStyle style) {
        style_ = style;
        setBubbleText(text_);
    }

    void setBubbleText(const QString& text) {
        text_ = text;
        document_.setDefaultFont(base_font_);
        document_.setPlainText(text);
        document_.setTextWidth(-1);

        const Metrics metrics = metricsFor(style_);
        QTextOption text_option = document_.defaultTextOption();
        text_option.setAlignment(style_ == BubbleStyle::Panel
            ? Qt::AlignLeft
            : Qt::AlignHCenter);
        document_.setDefaultTextOption(text_option);
        const int natural_width = static_cast<int>(std::ceil(document_.idealWidth()));
        int horizontal_padding = metrics.horizontal_padding;
        int content_width = 1;
        // A pill's top and bottom corners consume more horizontal space as it
        // grows taller. Increase the text inset until the entire text box is
        // inside the pill, including multi-line first and last rows.
        for (int iteration = 0; iteration < 12; ++iteration) {
            const int minimum_content_width = std::max(1, metrics.minimum_width - horizontal_padding);
            const int maximum_content_width = std::max(
                minimum_content_width, metrics.maximum_width - horizontal_padding);
            content_width = std::clamp(natural_width, minimum_content_width, maximum_content_width);
            document_.setTextWidth(content_width);

            const int candidate_width = std::clamp(
                content_width + horizontal_padding, metrics.minimum_width, metrics.maximum_width);
            const int candidate_height = std::max(
                static_cast<int>(std::ceil(document_.size().height())) + metrics.vertical_padding + 2,
                metrics.minimum_height);
            const qreal radius = std::min(candidate_width, candidate_height) / 2.0;
            const qreal text_top = std::max(1.0, metrics.vertical_padding / 2.0);
            const qreal curve_inset = radius > text_top
                ? radius - std::sqrt(std::max(0.0, radius * radius -
                                                (radius - text_top) * (radius - text_top)))
                : 0.0;
            const int required_padding = std::max(
                metrics.horizontal_padding,
                2 * static_cast<int>(std::ceil(curve_inset + 8.0)));
            if (required_padding <= horizontal_padding) break;
            horizontal_padding = std::min(required_padding, metrics.maximum_width - 24);
        }
        const int minimum_content_width = std::max(1, metrics.minimum_width - horizontal_padding);
        const int maximum_content_width = std::max(
            minimum_content_width, metrics.maximum_width - horizontal_padding);
        content_width = std::clamp(natural_width, minimum_content_width, maximum_content_width);
        document_.setTextWidth(content_width);

        const int maximum_content_height = metrics.maximum_height - metrics.vertical_padding - 2;
        QFont fitted_font = document_.defaultFont();
        while (document_.size().height() > maximum_content_height && fitted_font.pointSize() > 7) {
            fitted_font.setPointSize(fitted_font.pointSize() - 1);
            document_.setDefaultFont(fitted_font);
            document_.setTextWidth(content_width);
        }

        const int bubble_height = std::max(
            static_cast<int>(std::ceil(document_.size().height())) + metrics.vertical_padding + 2,
            metrics.minimum_height);
        horizontal_padding_ = horizontal_padding;
        int final_width = std::clamp(content_width + horizontal_padding,
                                     metrics.minimum_width, metrics.maximum_width);
        if ((final_width & 1) != 0 && final_width < metrics.maximum_width) ++final_width;
        setFixedSize(final_width,
                     std::min(bubble_height, metrics.maximum_height));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const Metrics metrics = metricsFor(style_);
        const QRectF bubble_rect = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
        qreal radius = 12.0;
        QColor border;
        QColor background;
        QColor text_color;
        if (style_ == BubbleStyle::Capsule) {
            border = QColor(61, 96, 101);
            background = QColor(27, 45, 51);
            text_color = QColor(235, 250, 246);
            radius = std::min(28.0, height() / 2.0 - 1.0);
        } else if (style_ == BubbleStyle::Ring) {
            border = QColor(235, 236, 240, 78);
            background = QColor(78, 80, 86, 72);
            text_color = QColor(242, 242, 247);
            radius = std::max(1.0, std::min(width(), height()) / 2.0 - 1.0);
        } else {
            border = QColor(214, 220, 225);
            background = QColor(251, 252, 253);
            text_color = QColor(31, 37, 43);
            radius = 11.0;
        }
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(background);
        painter.drawRoundedRect(bubble_rect, radius, radius);

        if (style_ == BubbleStyle::Panel) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(69, 184, 158));
            painter.drawRoundedRect(QRectF(1.0, 12.0, 4.0, std::max(12, height() - 24)), 2.0, 2.0);
        }

        painter.save();
        const qreal vertical_offset = std::max(
            metrics.vertical_padding / 2.0,
            (height() - document_.size().height()) / 2.0);
        painter.translate(horizontal_padding_ / 2.0, vertical_offset);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, text_color);
        document_.documentLayout()->draw(&painter, context);
        painter.restore();
    }

private:
    struct Metrics {
        int minimum_width;
        int maximum_width;
        int minimum_height;
        int maximum_height;
        int horizontal_padding;
        int vertical_padding;
    };

    static Metrics metricsFor(BubbleStyle style) {
        switch (style) {
        case BubbleStyle::Capsule:
            return {220, 560, 64, 560, 40, 30};
        case BubbleStyle::Ring:
            return {132, 480, 32, 540, 56, 10};
        case BubbleStyle::Panel:
            return {300, 520, 58, 560, 48, 28};
        }
        return {300, 520, 58, 560, 48, 28};
    }

    QTextDocument document_;
    QFont base_font_;
    QString text_;
    BubbleStyle style_ = BubbleStyle::Ring;
    int horizontal_padding_ = 32;
};

class InputSettingsDialog final : public QDialog {
public:
    InputSettingsDialog(const VadSettings& values,
                        const QString& shortcut,
                        TextMode mode,
                        const std::vector<HotwordEntry>& hotwords,
                        QWidget* parent = nullptr)
        : QDialog(parent) {
        setWindowTitle(QStringLiteral("语音输入设置"));
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        resize(620, 500);
        setMinimumSize(580, 460);
        setStyleSheet(QStringLiteral(
            "QDialog { background: #F7F8FA; }"
            "QLabel { color: #202329; }"
            "QTabWidget::pane { border: 1px solid #DADDE2; border-radius: 6px; background: #FFFFFF; top: -1px; }"
            "QTabBar::tab { color: #676D77; padding: 8px 20px; background: #EEF0F3; border: 1px solid #DADDE2; border-bottom: none; }"
            "QTabBar::tab:selected { color: #202329; background: #FFFFFF; }"
            "QComboBox, QKeySequenceEdit, QDoubleSpinBox { min-height: 32px; padding: 0 9px; border: 1px solid #D7DAE0; border-radius: 5px; background: #FFFFFF; color: #30343A; }"
            "QComboBox:focus, QKeySequenceEdit:focus, QDoubleSpinBox:focus { border: 1px solid #356AE6; }"
            "QKeySequenceEdit:disabled, QKeySequenceEdit QLineEdit:disabled { background: #F0F2F5; color: #858B95; selection-background-color: #F0F2F5; selection-color: #858B95; }"
            "QSlider::groove:horizontal { height: 4px; background: #E2E5E9; border-radius: 2px; }"
            "QSlider::sub-page:horizontal { background: #E6A52C; border-radius: 2px; }"
            "QSlider::handle:horizontal { width: 16px; margin: -6px 0; background: white; border: 2px solid #E6A52C; border-radius: 8px; }"
            "QPushButton { min-width: 76px; min-height: 30px; border: 1px solid #D7DAE0; border-radius: 5px; background: #FFFFFF; color: #30343A; }"
            "QPushButton:hover { background: #F0F2F5; }"
            "QPushButton#primary { border: none; background: #202329; color: white; }"
            "QPushButton#primary:hover { background: #343840; }"
            "QPushButton#modeSegment { min-width: 88px; border-radius: 4px; background: #F0F2F5; }"
            "QPushButton#modeSegment:checked { border-color: #356AE6; background: #EAF0FF; color: #244FB7; }"
            "QToolButton { width: 30px; height: 30px; border: 1px solid #D7DAE0; border-radius: 5px; background: #FFFFFF; color: #30343A; }"
            "QToolButton:hover { background: #F0F2F5; }"
            "QTableWidget { border: 1px solid #DADDE2; border-radius: 5px; background: #FFFFFF; gridline-color: #ECEEF1; color: #30343A; }"
            "QTableWidget::item:selected { background: #EAF0FF; color: #202329; }"
            "QHeaderView::section { padding: 7px; border: none; border-bottom: 1px solid #DADDE2; background: #F5F6F8; color: #676D77; }"));

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(24, 20, 24, 20);
        root->setSpacing(16);

        auto* heading = new QLabel(QStringLiteral("语音输入设置"));
        QFont heading_font = heading->font();
        heading_font.setPointSize(13);
        heading_font.setWeight(QFont::DemiBold);
        heading->setFont(heading_font);
        root->addWidget(heading);

        auto* tabs = new QTabWidget;
        tabs->addTab(createInputPage(shortcut, mode), QStringLiteral("输入"));
        tabs->addTab(createHotwordPage(hotwords), QStringLiteral("热词"));
        tabs->addTab(createVadPage(values), QStringLiteral("VAD"));
        root->addWidget(tabs);

        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        auto* reset_button = new QPushButton(QStringLiteral("恢复默认"));
        auto* cancel_button = new QPushButton(QStringLiteral("取消"));
        auto* apply_button = new QPushButton(QStringLiteral("应用"));
        apply_button->setObjectName(QStringLiteral("primary"));
        buttons->addWidget(reset_button);
        buttons->addStretch();
        buttons->addWidget(cancel_button);
        buttons->addWidget(apply_button);
        root->addLayout(buttons);

        connect(reset_button, &QPushButton::clicked, this, [this] {
            endpoint_slider_->setValue(700);
            threshold_slider_->setValue(55);
            minimum_db_slider_->setValue(-60);
            snr_slider_->setValue(3);
            const int default_index = shortcut_preset_->findData(QString::fromLatin1(default_hotkey));
            if (default_index >= 0) shortcut_preset_->setCurrentIndex(default_index);
            clean_mode_button_->setChecked(true);
        });
        connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
        connect(apply_button, &QPushButton::clicked, this, [this] {
            if (!hasUsableShortcutKey(this->shortcut())) {
                shortcut_error_->setText(QStringLiteral("请录入一个包含普通按键的组合，例如 Ctrl + Alt + Space。"));
                return;
            }
            accept();
        });
    }

    VadSettings values() const {
        return VadSettings{
            .endpoint_ms = endpoint_slider_->value(),
            .threshold_percent = threshold_slider_->value(),
            .minimum_db = minimum_db_slider_->value(),
            .snr_db = snr_slider_->value(),
        };
    }

    QString shortcut() const {
        const QString preset = shortcut_preset_->currentData().toString();
        if (preset != QStringLiteral("custom")) return preset;
        return canonicalShortcut(shortcut_edit_->keySequence().toString(QKeySequence::PortableText));
    }

    TextMode mode() const {
        return mode_group_->checkedId() == 0 ? TextMode::Raw : TextMode::Clean;
    }

    std::vector<HotwordEntry> hotwords() const {
        std::vector<HotwordEntry> entries;
        entries.reserve(static_cast<std::size_t>(hotword_table_->rowCount()));
        for (int row = 0; row < hotword_table_->rowCount(); ++row) {
            const QTableWidgetItem* phrase_item = hotword_table_->item(row, 1);
            const QString phrase = phrase_item == nullptr ? QString{} : phrase_item->text().trimmed();
            if (phrase.isEmpty()) continue;

            HotwordEntry entry;
            entry.phrase = to_utf8_string(phrase);
            if (const auto* enabled = qobject_cast<QCheckBox*>(hotword_table_->cellWidget(row, 0))) {
                entry.enabled = enabled->isChecked();
            }
            const QTableWidgetItem* aliases_item = hotword_table_->item(row, 2);
            const QString aliases_text = aliases_item == nullptr ? QString{} : aliases_item->text();
            for (const QString& alias : aliases_text.split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
                const QString trimmed = alias.trimmed();
                if (!trimmed.isEmpty()) entry.aliases.push_back(to_utf8_string(trimmed));
            }
            if (const auto* boost = qobject_cast<QDoubleSpinBox*>(hotword_table_->cellWidget(row, 3))) {
                entry.boost = static_cast<float>(boost->value());
            }
            const QTableWidgetItem* hits_item = hotword_table_->item(row, 4);
            if (hits_item != nullptr) entry.hits = hits_item->text().toULongLong();
            entries.push_back(std::move(entry));
        }
        return entries;
    }

private:
    enum class ValueFormat { Milliseconds, Threshold, Dbfs, Db };

    QWidget* createInputPage(const QString& current_shortcut, TextMode mode) {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 18, 18, 16);
        layout->setSpacing(12);

        auto* mode_label = new QLabel(QStringLiteral("输出方式"));
        QFont mode_font = mode_label->font();
        mode_font.setWeight(QFont::DemiBold);
        mode_label->setFont(mode_font);
        layout->addWidget(mode_label);

        auto* mode_row = new QHBoxLayout;
        mode_row->setSpacing(6);
        mode_group_ = new QButtonGroup(this);
        raw_mode_button_ = new QPushButton(QStringLiteral("原文"));
        clean_mode_button_ = new QPushButton(QStringLiteral("精简"));
        for (QPushButton* button : {raw_mode_button_, clean_mode_button_}) {
            button->setObjectName(QStringLiteral("modeSegment"));
            button->setCheckable(true);
            mode_row->addWidget(button);
        }
        mode_group_->addButton(raw_mode_button_, 0);
        mode_group_->addButton(clean_mode_button_, 1);
        (mode == TextMode::Raw ? raw_mode_button_ : clean_mode_button_)->setChecked(true);
        mode_row->addStretch();
        layout->addLayout(mode_row);

        layout->addSpacing(4);

        auto* label = new QLabel(QStringLiteral("按住开始，松开完成"));
        QFont label_font = label->font();
        label_font.setWeight(QFont::DemiBold);
        label->setFont(label_font);
        layout->addWidget(label);

        shortcut_preset_ = new QComboBox;
        shortcut_preset_->addItem(QStringLiteral("Ctrl + Alt + Space（推荐）"),
                                  QString::fromLatin1(default_hotkey));
        shortcut_preset_->addItem(QStringLiteral("Ctrl + Win"),
                                  QString::fromLatin1(control_windows_hotkey));
        shortcut_preset_->addItem(QStringLiteral("Ctrl + Shift + Space"),
                                  QStringLiteral("Ctrl+Shift+Space"));
        shortcut_preset_->addItem(QStringLiteral("F8"), QStringLiteral("F8"));
        shortcut_preset_->addItem(QStringLiteral("自定义"), QStringLiteral("custom"));
        layout->addWidget(shortcut_preset_);

        shortcut_edit_ = new QKeySequenceEdit;
        shortcut_edit_->setMaximumSequenceLength(1);
        shortcut_edit_->setToolTip(QStringLiteral("点击后按下一个组合键"));
        layout->addWidget(shortcut_edit_);

        shortcut_error_ = new QLabel;
        shortcut_error_->setWordWrap(true);
        shortcut_error_->setStyleSheet(QStringLiteral("color: #C43B3B;"));
        layout->addWidget(shortcut_error_);
        layout->addStretch();

        const QString canonical = canonicalShortcut(current_shortcut);
        const int preset_index = shortcut_preset_->findData(canonical);
        if (preset_index >= 0) {
            shortcut_preset_->setCurrentIndex(preset_index);
        } else {
            shortcut_preset_->setCurrentIndex(shortcut_preset_->findData(QStringLiteral("custom")));
            shortcut_edit_->setKeySequence(QKeySequence::fromString(canonical, QKeySequence::PortableText));
        }
        updateShortcutEditor();

        connect(shortcut_preset_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) {
                    shortcut_error_->clear();
                    updateShortcutEditor();
                });
        connect(shortcut_edit_, &QKeySequenceEdit::keySequenceChanged, this,
                [this](const QKeySequence&) { shortcut_error_->clear(); });
        return page;
    }

    QWidget* createHotwordPage(const std::vector<HotwordEntry>& entries) {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(8);

        hotword_table_ = new QTableWidget;
        hotword_table_->setColumnCount(5);
        hotword_table_->setHorizontalHeaderLabels({
            QStringLiteral("启用"),
            QStringLiteral("标准词"),
            QStringLiteral("别名（用 | 分隔）"),
            QStringLiteral("增强"),
            QStringLiteral("命中"),
        });
        hotword_table_->verticalHeader()->setVisible(false);
        hotword_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        hotword_table_->setSelectionMode(QAbstractItemView::SingleSelection);
        hotword_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        hotword_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        hotword_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        hotword_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        hotword_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        for (const HotwordEntry& entry : entries) addHotwordRow(entry);
        layout->addWidget(hotword_table_);

        auto* controls = new QHBoxLayout;
        controls->setSpacing(6);
        auto* add_button = new QToolButton;
        add_button->setText(QStringLiteral("+"));
        add_button->setToolTip(QStringLiteral("添加热词"));
        auto* remove_button = new QToolButton;
        remove_button->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
        remove_button->setToolTip(QStringLiteral("删除所选热词"));
        controls->addWidget(add_button);
        controls->addWidget(remove_button);
        controls->addStretch();
        layout->addLayout(controls);

        connect(add_button, &QToolButton::clicked, this, [this] {
            addHotwordRow(HotwordEntry{});
            const int row = hotword_table_->rowCount() - 1;
            hotword_table_->setCurrentCell(row, 1);
            hotword_table_->editItem(hotword_table_->item(row, 1));
        });
        connect(remove_button, &QToolButton::clicked, this, [this] {
            const int row = hotword_table_->currentRow();
            if (row >= 0) hotword_table_->removeRow(row);
        });
        return page;
    }

    void addHotwordRow(const HotwordEntry& entry) {
        const int row = hotword_table_->rowCount();
        hotword_table_->insertRow(row);

        auto* enabled = new QCheckBox;
        enabled->setChecked(entry.enabled);
        enabled->setToolTip(QStringLiteral("启用热词"));
        hotword_table_->setCellWidget(row, 0, enabled);

        auto* phrase = new QTableWidgetItem(to_qstring(entry.phrase));
        hotword_table_->setItem(row, 1, phrase);

        QStringList aliases;
        for (const std::string& alias : entry.aliases) aliases.push_back(to_qstring(alias));
        hotword_table_->setItem(row, 2, new QTableWidgetItem(aliases.join(QLatin1Char('|'))));

        auto* boost = new QDoubleSpinBox;
        boost->setRange(0.0, 12.0);
        boost->setDecimals(1);
        boost->setSingleStep(0.5);
        boost->setValue(entry.boost);
        boost->setToolTip(QStringLiteral("CTC 热词增强强度"));
        hotword_table_->setCellWidget(row, 3, boost);

        auto* hits = new QTableWidgetItem(QString::number(entry.hits));
        hits->setFlags(hits->flags() & ~Qt::ItemIsEditable);
        hits->setTextAlignment(Qt::AlignCenter);
        hotword_table_->setItem(row, 4, hits);
    }

    QWidget* createVadPage(const VadSettings& values) {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 18, 18, 16);
        layout->setSpacing(15);
        endpoint_slider_ = addSlider(layout, QStringLiteral("句尾静音"), 300, 2000, 100,
                                     values.endpoint_ms, ValueFormat::Milliseconds);
        threshold_slider_ = addSlider(layout, QStringLiteral("模型阈值"), 5, 95, 5,
                                      values.threshold_percent, ValueFormat::Threshold);
        minimum_db_slider_ = addSlider(layout, QStringLiteral("最低响度"), -80, -20, 1,
                                       values.minimum_db, ValueFormat::Dbfs);
        snr_slider_ = addSlider(layout, QStringLiteral("底噪余量"), 0, 20, 1,
                                values.snr_db, ValueFormat::Db);
        layout->addStretch();
        return page;
    }

    void updateShortcutEditor() {
        const QString value = shortcut_preset_->currentData().toString();
        const bool custom = value == QStringLiteral("custom");
        shortcut_edit_->setEnabled(custom);
        if (!custom) {
            shortcut_edit_->setKeySequence(
                QKeySequence::fromString(value, QKeySequence::PortableText));
        }
    }

    QSlider* addSlider(QVBoxLayout* root,
                       const QString& label_text,
                       int minimum,
                       int maximum,
                       int step,
                       int value,
                       ValueFormat format) {
        auto* container = new QWidget;
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        auto* labels = new QHBoxLayout;
        auto* label = new QLabel(label_text);
        auto* value_label = new QLabel;
        value_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value_label->setStyleSheet(QStringLiteral("color: #69707A;"));
        labels->addWidget(label);
        labels->addStretch();
        labels->addWidget(value_label);
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(minimum, maximum);
        slider->setSingleStep(step);
        slider->setPageStep(step);
        layout->addLayout(labels);
        layout->addWidget(slider);
        root->addWidget(container);

        const auto update_value = [value_label, format](int current) {
            switch (format) {
            case ValueFormat::Milliseconds:
                value_label->setText(QString::number(current) + QStringLiteral(" ms"));
                break;
            case ValueFormat::Threshold:
                value_label->setText(QString::number(current / 100.0, 'f', 2));
                break;
            case ValueFormat::Dbfs:
                value_label->setText(QString::number(current) + QStringLiteral(" dBFS"));
                break;
            case ValueFormat::Db:
                value_label->setText(QString::number(current) + QStringLiteral(" dB"));
                break;
            }
        };
        connect(slider, &QSlider::valueChanged, this, update_value);
        slider->setValue(value);
        update_value(slider->value());
        return slider;
    }

    QSlider* endpoint_slider_ = nullptr;
    QSlider* threshold_slider_ = nullptr;
    QSlider* minimum_db_slider_ = nullptr;
    QSlider* snr_slider_ = nullptr;
    QComboBox* shortcut_preset_ = nullptr;
    QKeySequenceEdit* shortcut_edit_ = nullptr;
    QLabel* shortcut_error_ = nullptr;
    QButtonGroup* mode_group_ = nullptr;
    QPushButton* raw_mode_button_ = nullptr;
    QPushButton* clean_mode_button_ = nullptr;
    QTableWidget* hotword_table_ = nullptr;
};

class VoiceInputWindow final : public QWidget {
public:
    explicit VoiceInputWindow(BubbleStyle bubble_style = BubbleStyle::Ring,
                              bool preview_mode = false)
        : settings_(QStringLiteral("SenseVoice"), QStringLiteral("LocalDictation")),
          bubble_style_(bubble_style), preview_mode_(preview_mode) {
        setWindowTitle(QStringLiteral("SenseVoice 语音输入"));
        setWindowIcon(sensevoiceIcon());
        // Keep the overlay frameless and always-on-top, but use a normal top-level
        // window so Windows creates a taskbar button with the application icon.
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        size_animation_.setDuration(140);
        size_animation_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&size_animation_, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& value) {
                    if (!isVisible()) return;
                    const QSize animated_size = value.toSize();
                    if (!animated_size.isValid()) return;
                    const int anchor_left = geometry_anchor_valid_
                        ? geometry_anchor_left_ : x();
                    const int anchor_bottom = geometry_anchor_valid_
                        ? geometry_anchor_bottom_ : y() + height();
                    setGeometry(anchor_left,
                                anchor_bottom - animated_size.height(),
                                animated_size.width(),
                                animated_size.height());
                });
        if (preview_mode_) {
            setWindowFlag(Qt::WindowDoesNotAcceptFocus, false);
            setAttribute(Qt::WA_ShowWithoutActivating, false);
            setFocusPolicy(Qt::StrongFocus);
        }
        resize(bubble_minimum_width + window_margin * 2,
               bubble_minimum_height + 48 + control_spacing + window_margin * 2);
        setFocusPolicy(Qt::NoFocus);
        loadSettings();
        buildUi();
        if (!preview_mode_) buildTrayMenu();
        centerNearBottom();

        meter_timer_.setInterval(50);
        connect(&meter_timer_, &QTimer::timeout, this, [this] { updateMeter(); });
        meter_timer_.start();
        status_reset_timer_.setSingleShot(true);
        connect(&status_reset_timer_, &QTimer::timeout, this, [this] {
            if (state_ == State::Ready) {
                if (committed_text_.isEmpty()) setBubbleStatus(
                    QStringLiteral("按住 %1").arg(shortcutDisplayName(hotkey_shortcut_)));
                else refreshTranscript();
            }
        });
#ifdef _WIN32
        if (!preview_mode_) {
            hotkey_hold_timer_.setSingleShot(true);
            connect(&hotkey_hold_timer_, &QTimer::timeout, this, [this] { beginHotkeySession(); });
            hotkey_release_timer_.setInterval(20);
            connect(&hotkey_release_timer_, &QTimer::timeout, this, [this] { pollHotkeyRelease(); });
            rebuildHotkeyBinding();
            registerHotkeys();
            installKeyboardHook();
        }
#endif
        if (preview_mode_) {
            state_ = State::Ready;
            recording_control_->setEnabled(true);
            setBubbleStatus(QStringLiteral("这一句用于比较浮窗方案的文字布局。说长一点时，气泡会自动扩展，不滚动，也不会裁切内容。"));
        } else {
            beginLoadModels();
        }
    }

    ~VoiceInputWindow() override {
        shutting_down_.store(true, std::memory_order_release);
        meter_timer_.stop();
        status_reset_timer_.stop();
#ifdef _WIN32
        hotkey_hold_timer_.stop();
        hotkey_release_timer_.stop();
        uninstallKeyboardHook();
        unregisterHotkeys();
#endif

        if (stopper_.joinable()) {
            stopper_.join();
        } else if (microphone_ != nullptr && recognizer_ != nullptr) {
            microphone_->stop();
            recognizer_->cancel();
        }
        microphone_.reset();
        recognizer_.reset();
        if (loader_.joinable()) loader_.join();
    }

    void hideUntilInput() {
        hide();
    }

    void setPreviewContent(const QString& text) {
        if (!preview_mode_) return;
        state_ = State::Ready;
        recording_control_->setEnabled(true);
        setBubbleStatus(text);
    }

    void setPreviewSignal() {
        if (!preview_mode_) return;
        meter_timer_.stop();
        recording_control_->setPreviewSignal();
    }

    void startPreviewGeometryTest() {
        if (!preview_mode_) return;
        setPreviewGeometryStep(0);
    }

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
#ifdef _WIN32
        applyGlassBackdrop();
#endif
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
#ifdef _WIN32
        updateGlassMask();
#endif
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            dragging_ = true;
            drag_offset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) {
            move(event->globalPosition().toPoint() - drag_offset_);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging_) {
            geometry_anchor_left_ = x();
            geometry_anchor_bottom_ = y() + height();
            geometry_anchor_valid_ = true;
        }
        dragging_ = false;
        QWidget::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);
        QAction* settings_action = menu.addAction(QStringLiteral("设置..."));
        QAction* cancel_action = menu.addAction(QStringLiteral("取消本次输入"));
        cancel_action->setEnabled(state_ == State::Listening);
        QAction* copy_action = menu.addAction(QStringLiteral("复制当前文字"));
        copy_action->setEnabled(!currentTranscript().trimmed().isEmpty());
        QMenu* recent_menu = menu.addMenu(QStringLiteral("最近输入"));
        populateRecentMenu(recent_menu);
        menu.addSeparator();
        QAction* exit_action = menu.addAction(QStringLiteral("退出"));
        QAction* selected = menu.exec(event->globalPos());
        if (selected == settings_action) openSettings();
        else if (selected == cancel_action) stopSession(false);
        else if (selected == copy_action) {
            QApplication::clipboard()->setText(currentTranscript().trimmed());
            setTransientStatus(QStringLiteral("已复制当前文字"));
        }
        else if (selected == exit_action) close();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape && state_ == State::Listening) {
            stopSession(false);
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void closeEvent(QCloseEvent* event) override {
        event->accept();
        if (preview_mode_) QCoreApplication::quit();
    }

#ifdef _WIN32
    bool nativeEvent(const QByteArray& event_type, void* message, qintptr* result) override {
        (void)event_type;
        auto* native_message = static_cast<MSG*>(message);
        if (native_message->message == WM_HOTKEY &&
            (native_message->wParam == hotkey_primary_id ||
             native_message->wParam == hotkey_left_id ||
             native_message->wParam == hotkey_right_id)) {
            if (result != nullptr) *result = 0;
            handleHotkeyPressed();
            return true;
        }
        if (native_message->message == hotkey_state_message) {
            (void)native_message->wParam;
            (void)native_message->lParam;
            if (hotkeyKeysDown()) handleHotkeyPressed();
            else if (hotkey_pending_ || hotkey_recording_) pollHotkeyRelease();
            if (result != nullptr) *result = 0;
            return true;
        }
        return QWidget::nativeEvent(event_type, message, result);
    }
#endif

private:
    enum class State { Loading, Ready, Listening, Stopping, Error };

    void setPreviewGeometryStep(int step) {
        static const std::array<QString, 6> samples = {
            QStringLiteral("短句"),
            QStringLiteral("文字逐步变长"),
            QStringLiteral("文字逐步变长时窗口左边缘应该保持不动"),
            QStringLiteral("文字逐步变长时窗口左边缘应该保持不动，底边也应该保持不动。"),
            QStringLiteral("这是一个更长的预览句子，用来验证窗口宽高动画不会导致水平抖动。"),
            QStringLiteral("这是一个更长的预览句子，用来验证窗口宽高动画不会导致水平抖动，内容变成多行后仍然保持稳定。"),
        };
        if (step >= static_cast<int>(samples.size())) return;
        setPreviewContent(samples[static_cast<std::size_t>(step)]);
        if (step + 1 < static_cast<int>(samples.size())) {
            QTimer::singleShot(180, this, [this, step] {
                setPreviewGeometryStep(step + 1);
            });
        }
    }

    void buildUi() {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(window_margin, window_margin, window_margin, window_margin);
        root->setSpacing(control_spacing);
        root->setAlignment(Qt::AlignHCenter);

        transcript_ = new TranscriptBubble(bubble_style_);
        setBubbleStatus(QStringLiteral("正在加载..."));

        recording_control_ = new RecordingControl(bubble_style_);
        recording_control_->setEnabled(false);
        recording_control_->setMode(text_mode_);
        root->addWidget(transcript_, 0, Qt::AlignHCenter);
        root->addWidget(recording_control_, 0, Qt::AlignHCenter);

        recording_control_->setHandlers(
            [this] {
                if (state_ == State::Ready) startSession(false);
                else if (state_ == State::Listening) stopSession(true);
            },
            [this] {
                if (state_ == State::Listening) stopSession(false);
            });
    }

    void buildTrayMenu() {
        tray_icon_ = new QSystemTrayIcon(this);
        tray_icon_->setIcon(sensevoiceIcon());
        tray_icon_->setToolTip(QStringLiteral("SenseVoice 语音输入"));
        auto* tray_menu = new QMenu(this);
        QAction* settings_action = tray_menu->addAction(QStringLiteral("设置..."));
        QAction* show_action = tray_menu->addAction(QStringLiteral("显示输入窗"));
        history_menu_ = tray_menu->addMenu(QStringLiteral("最近输入"));
        populateRecentMenu(history_menu_);
        tray_menu->addSeparator();
        QAction* exit_action = tray_menu->addAction(QStringLiteral("退出"));
        connect(settings_action, &QAction::triggered, this, [this] { openSettings(); });
        connect(show_action, &QAction::triggered, this, [this] {
            showPopup();
            setBubbleStatus(QStringLiteral("按住 %1")
                                .arg(shortcutDisplayName(hotkey_shortcut_)));
        });
        connect(exit_action, &QAction::triggered, this, [this] {
            close();
            QCoreApplication::quit();
        });
        tray_icon_->setContextMenu(tray_menu);
        connect(tray_icon_, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger ||
                        reason == QSystemTrayIcon::DoubleClick) {
                        showPopup();
                    }
                });
        tray_icon_->show();
    }

    void populateRecentMenu(QMenu* menu) {
        if (menu == nullptr) return;
        menu->clear();
        const QStringList entries = settings_.value(QStringLiteral("history/entries")).toStringList();
        if (entries.isEmpty()) {
            QAction* empty_action = menu->addAction(QStringLiteral("暂无记录"));
            empty_action->setEnabled(false);
            return;
        }
        for (const QString& entry : entries) {
            QString label = entry.simplified();
            if (label.size() > 32) label = label.left(32) + QStringLiteral("...");
            QAction* action = menu->addAction(label);
            action->setToolTip(entry);
            connect(action, &QAction::triggered, this, [this, entry] {
                QApplication::clipboard()->setText(entry);
                setTransientStatus(QStringLiteral("已复制最近输入"));
            });
        }
    }

    void recordHistory(const QString& text) {
        QStringList entries = settings_.value(QStringLiteral("history/entries")).toStringList();
        entries.prepend(text);
        while (entries.size() > 20) entries.removeLast();
        settings_.setValue(QStringLiteral("history/entries"), entries);
        settings_.sync();
        populateRecentMenu(history_menu_);
    }

    void showPopup() {
        if (isVisible()) return;
        geometry_anchor_valid_ = false;
        centerNearBottom();
        show();
    }

    void hidePopup() {
        if (state_ == State::Listening || state_ == State::Stopping) return;
        size_animation_.stop();
        hide();
        geometry_anchor_valid_ = false;
    }

    void loadSettings() {
        vad_settings_.endpoint_ms = settings_.value(QStringLiteral("vad/endpoint_ms"), 700).toInt();
        vad_settings_.threshold_percent = settings_.value(QStringLiteral("vad/threshold_percent"), 55).toInt();
        vad_settings_.minimum_db = settings_.value(QStringLiteral("vad/minimum_db"), -60).toInt();
        vad_settings_.snr_db = settings_.value(QStringLiteral("vad/snr_db"), 3).toInt();
        text_mode_ = settings_.value(QStringLiteral("text/mode"), 1).toInt() == 0
            ? TextMode::Raw
            : TextMode::Clean;
        const int profile_version = settings_.value(QStringLiteral("vad/profile_version"), 0).toInt();
        if (profile_version < 4 && vad_settings_.endpoint_ms == 700 &&
            vad_settings_.threshold_percent == 80 && vad_settings_.minimum_db == -45 &&
            vad_settings_.snr_db == 8) {
            vad_settings_ = {};
            settings_.setValue(QStringLiteral("vad/endpoint_ms"), vad_settings_.endpoint_ms);
            settings_.setValue(QStringLiteral("vad/threshold_percent"), vad_settings_.threshold_percent);
            settings_.setValue(QStringLiteral("vad/minimum_db"), vad_settings_.minimum_db);
            settings_.setValue(QStringLiteral("vad/snr_db"), vad_settings_.snr_db);
        }
        settings_.setValue(QStringLiteral("vad/profile_version"), 4);
        settings_.sync();
        hotkey_shortcut_ = canonicalShortcut(settings_.value(
            QStringLiteral("hotkey/shortcut"), QString::fromLatin1(default_hotkey)).toString());
        if (!hasUsableShortcutKey(hotkey_shortcut_)) {
            hotkey_shortcut_ = QString::fromLatin1(default_hotkey);
        }
    }

    void saveSettings() {
        settings_.setValue(QStringLiteral("vad/endpoint_ms"), vad_settings_.endpoint_ms);
        settings_.setValue(QStringLiteral("vad/threshold_percent"), vad_settings_.threshold_percent);
        settings_.setValue(QStringLiteral("vad/minimum_db"), vad_settings_.minimum_db);
        settings_.setValue(QStringLiteral("vad/snr_db"), vad_settings_.snr_db);
        settings_.setValue(QStringLiteral("hotkey/shortcut"), hotkey_shortcut_);
        settings_.setValue(QStringLiteral("text/mode"), text_mode_ == TextMode::Raw ? 0 : 1);
        settings_.sync();
    }

    void openSettings() {
        if (state_ == State::Listening || state_ == State::Stopping) return;
        if (state_ == State::Loading) {
            setTransientStatus(QStringLiteral("模型加载中"));
            return;
        }
        InputSettingsDialog dialog(
            vad_settings_, hotkey_shortcut_, text_mode_, text_processor_.hotwords(), this);
        if (dialog.exec() == QDialog::Accepted) {
            vad_settings_ = dialog.values();
            text_mode_ = dialog.mode();
            recording_control_->setMode(text_mode_);
            text_processor_.set_hotwords(dialog.hotwords());
            const QString selected_shortcut = dialog.shortcut();
#ifdef _WIN32
            applyHotkey(selected_shortcut);
#else
            hotkey_shortcut_ = selected_shortcut;
#endif
            saveSettings();
            std::string hotword_error;
            const std::filesystem::path hotword_path =
                std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
                L"hotwords.tsv";
            if (!text_processor_.save_hotwords(hotword_path, hotword_error)) {
                QMessageBox::warning(this, QStringLiteral("热词未保存"), to_qstring(hotword_error));
            }
            setTransientStatus(QStringLiteral("已应用：%1")
                                   .arg(shortcutDisplayName(hotkey_shortcut_)));
            QTimer::singleShot(1600, this, [this] { hidePopup(); });
        }
    }

    void centerNearBottom() {
        QScreen* screen = QGuiApplication::primaryScreen();
        if (screen == nullptr) return;
        const QRect area = screen->availableGeometry();
        move(area.center().x() - width() / 2, area.bottom() - height() - 72);
    }

#ifdef _WIN32
    static QRegion roundedRegion(const QRect& rect, int radius) {
        if (rect.isEmpty()) return {};
        const int clamped_radius = std::clamp(
            radius, 0, std::min(rect.width(), rect.height()) / 2);
        if (clamped_radius == 0) return QRegion(rect);

        const int diameter = clamped_radius * 2;
        QRegion region(rect.adjusted(clamped_radius, 0, -clamped_radius, 0));
        region += QRegion(rect.adjusted(0, clamped_radius, 0, -clamped_radius));
        region += QRegion(QRect(rect.left(), rect.top(), diameter, diameter), QRegion::Ellipse);
        region += QRegion(QRect(rect.right() - diameter + 1, rect.top(), diameter, diameter),
                          QRegion::Ellipse);
        region += QRegion(QRect(rect.left(), rect.bottom() - diameter + 1, diameter, diameter),
                          QRegion::Ellipse);
        region += QRegion(QRect(rect.right() - diameter + 1,
                                rect.bottom() - diameter + 1,
                                diameter,
                                diameter),
                          QRegion::Ellipse);
        return region;
    }

    void updateGlassMask() {
        if (transcript_ == nullptr || recording_control_ == nullptr) return;
        if (layout() != nullptr) layout()->activate();
        QRegion glass_region = roundedRegion(
            transcript_->geometry(), std::min(transcript_->width(), transcript_->height()) / 2);
        glass_region += roundedRegion(
            recording_control_->geometry(), recording_control_->height() / 2);
        setMask(glass_region);
    }

    void applyGlassBackdrop() {
        const HWND window_handle = reinterpret_cast<HWND>(winId());
        // BlurBehind respects the window region set by updateGlassMask().
        // Unlike a system backdrop, it does not paint the transparent gap
        // between the transcript and the control capsule.
        if (HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll")) {
            const auto enable_blur = reinterpret_cast<DwmEnableBlurBehindWindowFn>(
                GetProcAddress(dwmapi, "DwmEnableBlurBehindWindow"));
            if (enable_blur != nullptr) {
                constexpr DWORD blur_enable = 0x1;
                const DwmBlurBehind blur{blur_enable, TRUE, nullptr, FALSE};
                enable_blur(window_handle, &blur);
            }
            FreeLibrary(dwmapi);
        }
        updateGlassMask();
    }

    struct HotkeyBinding {
        UINT modifiers = 0;
        UINT virtual_key = 0;
        bool control_windows = false;
        bool valid = false;
    };

    static UINT virtualKeyForQtKey(Qt::Key key) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) return static_cast<UINT>(key);
        if (key >= Qt::Key_0 && key <= Qt::Key_9) return static_cast<UINT>(key);
        if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
            return VK_F1 + static_cast<UINT>(key - Qt::Key_F1);
        }
        switch (key) {
        case Qt::Key_Space: return VK_SPACE;
        case Qt::Key_Return:
        case Qt::Key_Enter: return VK_RETURN;
        case Qt::Key_Tab: return VK_TAB;
        case Qt::Key_Backspace: return VK_BACK;
        case Qt::Key_Delete: return VK_DELETE;
        case Qt::Key_Insert: return VK_INSERT;
        case Qt::Key_Escape: return VK_ESCAPE;
        case Qt::Key_Left: return VK_LEFT;
        case Qt::Key_Right: return VK_RIGHT;
        case Qt::Key_Up: return VK_UP;
        case Qt::Key_Down: return VK_DOWN;
        case Qt::Key_Home: return VK_HOME;
        case Qt::Key_End: return VK_END;
        case Qt::Key_PageUp: return VK_PRIOR;
        case Qt::Key_PageDown: return VK_NEXT;
        case Qt::Key_Comma: return VK_OEM_COMMA;
        case Qt::Key_Period: return VK_OEM_PERIOD;
        case Qt::Key_Slash: return VK_OEM_2;
        case Qt::Key_Semicolon: return VK_OEM_1;
        case Qt::Key_Minus: return VK_OEM_MINUS;
        case Qt::Key_Equal: return VK_OEM_PLUS;
        case Qt::Key_BracketLeft: return VK_OEM_4;
        case Qt::Key_BracketRight: return VK_OEM_6;
        case Qt::Key_Backslash: return VK_OEM_5;
        case Qt::Key_Apostrophe: return VK_OEM_7;
        case Qt::Key_QuoteLeft: return VK_OEM_3;
        default: return 0;
        }
    }

    void rebuildHotkeyBinding() {
        hotkey_binding_ = {};
        if (hotkey_shortcut_ == QString::fromLatin1(control_windows_hotkey)) {
            hotkey_binding_.modifiers = MOD_CONTROL;
            hotkey_binding_.virtual_key = VK_LWIN;
            hotkey_binding_.control_windows = true;
            hotkey_binding_.valid = true;
            return;
        }

        const QKeySequence sequence = QKeySequence::fromString(
            hotkey_shortcut_, QKeySequence::PortableText);
        if (sequence.isEmpty() || sequence.count() != 1) return;
        const QKeyCombination combination = sequence[0];
        hotkey_binding_.virtual_key = virtualKeyForQtKey(combination.key());
        if (hotkey_binding_.virtual_key == 0) return;
        const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
        if (modifiers.testFlag(Qt::ControlModifier)) hotkey_binding_.modifiers |= MOD_CONTROL;
        if (modifiers.testFlag(Qt::AltModifier)) hotkey_binding_.modifiers |= MOD_ALT;
        if (modifiers.testFlag(Qt::ShiftModifier)) hotkey_binding_.modifiers |= MOD_SHIFT;
        if (modifiers.testFlag(Qt::MetaModifier)) hotkey_binding_.modifiers |= MOD_WIN;
        hotkey_binding_.valid = true;
    }

    void registerHotkeys() {
        if (!hotkey_binding_.valid) return;
        const HWND handle = reinterpret_cast<HWND>(winId());
        if (hotkey_binding_.control_windows) {
            hotkey_left_registered_ = RegisterHotKey(
                handle, hotkey_left_id, MOD_CONTROL | MOD_NOREPEAT, VK_LWIN) != FALSE;
            hotkey_right_registered_ = RegisterHotKey(
                handle, hotkey_right_id, MOD_CONTROL | MOD_NOREPEAT, VK_RWIN) != FALSE;
        } else {
            hotkey_primary_registered_ = RegisterHotKey(
                handle, hotkey_primary_id, hotkey_binding_.modifiers | MOD_NOREPEAT,
                hotkey_binding_.virtual_key) != FALSE;
        }
        const bool registered = hotkey_primary_registered_ || hotkey_left_registered_ ||
            hotkey_right_registered_;
        if (!registered) {
            recording_control_->setToolTip(QStringLiteral("系统快捷键被占用，已切换为后台按键监听"));
        }
    }

    void unregisterHotkeys() {
        const HWND handle = reinterpret_cast<HWND>(winId());
        if (hotkey_primary_registered_) UnregisterHotKey(handle, hotkey_primary_id);
        if (hotkey_left_registered_) UnregisterHotKey(handle, hotkey_left_id);
        if (hotkey_right_registered_) UnregisterHotKey(handle, hotkey_right_id);
        hotkey_primary_registered_ = false;
        hotkey_left_registered_ = false;
        hotkey_right_registered_ = false;
    }

    void applyHotkey(const QString& requested_shortcut) {
        const QString canonical = canonicalShortcut(requested_shortcut);
        if (!hasUsableShortcutKey(canonical)) return;
        if (canonical == hotkey_shortcut_) return;

        hotkey_hold_timer_.stop();
        hotkey_release_timer_.stop();
        hotkey_pending_ = false;
        hotkey_recording_ = false;
        unregisterHotkeys();
        hotkey_shortcut_ = canonical;
        rebuildHotkeyBinding();
        registerHotkeys();
    }

    static LRESULT CALLBACK keyboardHookProcedure(int code, WPARAM message, LPARAM data) {
        if (code == HC_ACTION && hotkey_message_window_ != nullptr) {
            const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            const bool released = message == WM_KEYUP || message == WM_SYSKEYUP;
            if ((pressed || released) && event->vkCode <= 0xFF) {
                PostMessageW(hotkey_message_window_, hotkey_state_message,
                             static_cast<WPARAM>(event->vkCode), pressed ? 1 : 0);
            }
        }
        return CallNextHookEx(keyboard_hook_, code, message, data);
    }

    void installKeyboardHook() {
        hotkey_message_window_ = reinterpret_cast<HWND>(winId());
        keyboard_hook_ = SetWindowsHookExW(
            WH_KEYBOARD_LL, keyboardHookProcedure, GetModuleHandleW(nullptr), 0);
        if (keyboard_hook_ == nullptr) {
            recording_control_->setToolTip(QStringLiteral("按住快捷键监听失败，请点击开始"));
        }
    }

    void uninstallKeyboardHook() {
        hotkey_message_window_ = nullptr;
        if (keyboard_hook_ != nullptr) {
            UnhookWindowsHookEx(keyboard_hook_);
            keyboard_hook_ = nullptr;
        }
    }

    bool hotkeyKeysDown() const {
        if (!hotkey_binding_.valid) return false;
        const auto key_down = [](int key) {
            return (GetAsyncKeyState(key) & 0x8000) != 0;
        };
        const auto control_down = [&key_down] {
            return key_down(VK_LCONTROL) || key_down(VK_RCONTROL);
        };
        const auto shift_down = [&key_down] {
            return key_down(VK_LSHIFT) || key_down(VK_RSHIFT);
        };
        const auto alt_down = [&key_down] {
            return key_down(VK_LMENU) || key_down(VK_RMENU);
        };
        const auto windows_down = [&key_down] {
            return key_down(VK_LWIN) || key_down(VK_RWIN);
        };

        if (hotkey_binding_.control_windows) {
            return control_down() && windows_down();
        }
        if ((hotkey_binding_.modifiers & MOD_CONTROL) != 0 && !control_down()) return false;
        if ((hotkey_binding_.modifiers & MOD_SHIFT) != 0 && !shift_down()) return false;
        if ((hotkey_binding_.modifiers & MOD_ALT) != 0 && !alt_down()) return false;
        if ((hotkey_binding_.modifiers & MOD_WIN) != 0 && !windows_down()) return false;
        return key_down(static_cast<int>(hotkey_binding_.virtual_key));
    }

    static bool usableInjectionTarget(const WindowsTextInputTarget& target) {
        if (!target.valid()) return false;
        const HWND window = reinterpret_cast<HWND>(target.window);
        return window != nullptr && IsWindow(window) != FALSE;
    }

    void selectInjectionTarget() {
        const WindowsTextInputTarget captured =
            capture_windows_text_input_target(GetCurrentProcessId());
        if (usableInjectionTarget(captured)) {
            target_ = captured;
        } else if (usableInjectionTarget(last_target_)) {
            target_ = last_target_;
        } else {
            target_ = {};
        }
        inject_on_complete_ = target_.valid();
    }

    void handleHotkeyPressed() {
        if (hotkey_pending_ || hotkey_recording_) return;
        if (state_ == State::Ready) selectInjectionTarget();
        showPopup();
        if (state_ != State::Ready) {
            if (state_ == State::Loading) setTransientStatus(QStringLiteral("模型加载中"));
            return;
        }
        hotkey_pending_ = true;
        setBubbleStatus(QStringLiteral("继续按住..."));
        hotkey_hold_timer_.start(280);
        hotkey_release_timer_.start();
    }

    void beginHotkeySession() {
        if (!hotkey_pending_) return;
        if (!hotkeyKeysDown()) {
            hotkey_pending_ = false;
            hotkey_release_timer_.stop();
            setTransientStatus(QStringLiteral("按住时间太短"));
            return;
        }
        hotkey_pending_ = false;
        hotkey_recording_ = true;
        startSession(true);
        if (state_ != State::Listening) hotkey_recording_ = false;
    }

    void pollHotkeyRelease() {
        if (hotkeyKeysDown()) return;
        if (hotkey_pending_) {
            hotkey_pending_ = false;
            hotkey_hold_timer_.stop();
            hotkey_release_timer_.stop();
            setTransientStatus(QStringLiteral("按住时间太短"));
            QTimer::singleShot(800, this, [this] { hidePopup(); });
            return;
        }
        if (hotkey_recording_) {
            hotkey_recording_ = false;
            hotkey_release_timer_.stop();
            if (state_ == State::Listening) stopSession(true);
            return;
        }
        hotkey_release_timer_.stop();
    }

    bool injectIntoTarget(WindowsTextInputTarget target, const QString& text) {
        if (!usableInjectionTarget(target) || text.trimmed().isEmpty()) return false;
        const std::wstring wide_text = text.toStdWString();
        return inject_text_into_windows_text_input(
            std::move(target),
            wide_text,
            GetCurrentProcessId());
    }
#endif

    void beginLoadModels() {
        loader_ = std::thread([this] {
            const std::filesystem::path directory =
                QCoreApplication::applicationDirPath().toStdWString();
            std::string error;
            bool success = engine_.load(directory / L"models" / L"sensevoice-small-q8.gguf", 8, error) &&
                vad_.load(directory / L"models" / L"fsmn-vad.gguf", 8, error);
            if (success && std::filesystem::exists(directory / L"hotwords.tsv")) {
                success = text_processor_.load_hotwords(directory / L"hotwords.tsv", error);
            }
            if (success && std::filesystem::exists(directory / L"corrections.tsv")) {
                success = text_processor_.load_correction_rules(directory / L"corrections.tsv", error);
            }
            if (success && std::filesystem::exists(directory / L"dict")) {
                success = text_processor_.initialize_segmenter(directory / L"dict", error);
            }
            if (shutting_down_.load(std::memory_order_acquire)) return;
            QMetaObject::invokeMethod(this, [this, success, error] {
                if (success) {
                    state_ = State::Ready;
                    setBubbleStatus(QStringLiteral("按住 %1")
                                        .arg(shortcutDisplayName(hotkey_shortcut_)));
                    recording_control_->setEnabled(true);
                } else {
                    state_ = State::Error;
                    setBubbleStatus(QStringLiteral("模型加载失败"));
                    recording_control_->setToolTip(to_qstring(error));
                }
            }, Qt::QueuedConnection);
        });
    }

    void startSession(bool via_hotkey) {
        if (state_ != State::Ready) return;
        if (stopper_.joinable()) stopper_.join();
        status_reset_timer_.stop();
        committed_text_.clear();
        partial_text_.clear();
        stop_should_commit_ = false;
#ifdef _WIN32
        if (!via_hotkey) {
            selectInjectionTarget();
        }
#else
        (void)via_hotkey;
        inject_on_complete_ = false;
#endif

        recognizer_ = std::make_unique<StreamRecognizer>(
            engine_,
            &vad_,
            StreamRecognizerConfig{
                .partial_interval_ms = 450,
                .minimum_audio_ms = 600,
                .minimum_new_audio_ms = 240,
                .endpoint_silence_ms = vad_settings_.endpoint_ms,
                .maximum_utterance_ms = 15'000,
                .memory_limit_mb = 300,
                .vad_speech_threshold = vad_settings_.threshold_percent / 100.0F,
                .vad_minimum_db = static_cast<float>(vad_settings_.minimum_db),
                .vad_minimum_snr_db = static_cast<float>(vad_settings_.snr_db),
                .vad_minimum_speech_ms = 200,
            },
            [this](const RecognitionEvent& event) {
                if (shutting_down_.load(std::memory_order_acquire)) return;
                QMetaObject::invokeMethod(this, [this, event] { handleRecognition(event); }, Qt::QueuedConnection);
            },
            &text_processor_);
        microphone_ = std::make_unique<MicrophoneCapture>();
        recognizer_->start();

        std::string error;
        if (!microphone_->start(
                [this](std::span<const float> samples) {
                    if (recognizer_ != nullptr) recognizer_->accept_pcm(samples);
                },
                error)) {
            recognizer_->cancel();
            recognizer_.reset();
            microphone_.reset();
            state_ = State::Ready;
            recording_control_->setListening(false);
            recording_control_->setEnabled(true);
            recording_control_->setToolTip(to_qstring(error));
#ifdef _WIN32
            hotkey_recording_ = false;
            hotkey_release_timer_.stop();
#endif
            setTransientStatus(QStringLiteral("麦克风不可用"));
            return;
        }

        state_ = State::Listening;
        session_elapsed_.start();
        setBubbleStatus(QStringLiteral("正在听取..."));
        beginStableSessionGeometry();
        recording_control_->setListening(true);
        recording_control_->setEnabled(true);
    }

    void stopSession(bool commit) {
        if (state_ != State::Listening || microphone_ == nullptr || recognizer_ == nullptr) return;
        state_ = State::Stopping;
        stop_should_commit_ = commit;
        if (!commit) inject_on_complete_ = false;
#ifdef _WIN32
        hotkey_recording_ = false;
        hotkey_release_timer_.stop();
#endif
        setBubbleStatus(commit ? QStringLiteral("正在完成...") : QStringLiteral("正在取消..."));
        recording_control_->setStopping(true);

        if (stopper_.joinable()) stopper_.join();
        stopper_ = std::thread([this, commit] {
            microphone_->stop();
            if (commit) recognizer_->finish();
            else recognizer_->cancel();
            if (shutting_down_.load(std::memory_order_acquire)) return;
            QMetaObject::invokeMethod(this, [this, commit] { stopWorkerFinished(commit); }, Qt::QueuedConnection);
        });
    }

    void stopWorkerFinished(bool commit) {
        if (stopper_.joinable()) stopper_.join();
        QTimer::singleShot(0, this, [this, commit] { sessionStopped(commit); });
    }

    void sessionStopped(bool commit) {
        microphone_.reset();
        recognizer_.reset();
        partial_text_.clear();

        QString final_text = committed_text_.trimmed();
        if (!commit) {
            committed_text_.clear();
        } else if (!final_text.isEmpty()) {
            if (text_mode_ == TextMode::Clean) {
                const QByteArray utf8 = final_text.toUtf8();
                final_text = to_qstring(TextProcessor::polish_dictation(
                    std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size()))));
            }
            committed_text_ = final_text;
            QApplication::clipboard()->setText(final_text);
            recordHistory(final_text);
            std::string hotword_error;
            const std::filesystem::path hotword_path =
                std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
                L"hotwords.tsv";
            if (!text_processor_.save_hotwords(hotword_path, hotword_error)) {
                recording_control_->setToolTip(to_qstring(hotword_error));
            }
        }

        state_ = State::Ready;
        stop_should_commit_ = false;
        recording_control_->setListening(false);
        recording_control_->setEnabled(true);
        if (!commit) {
#ifdef _WIN32
            target_ = {};
            inject_on_complete_ = false;
#endif
            setTransientStatus(QStringLiteral("已取消"));
            QTimer::singleShot(500, this, [this] { hidePopup(); });
        } else if (final_text.isEmpty()) {
#ifdef _WIN32
            target_ = {};
            inject_on_complete_ = false;
#endif
            setTransientStatus(QStringLiteral("未识别到"));
            QTimer::singleShot(500, this, [this] { hidePopup(); });
        } else {
#ifdef _WIN32
            WindowsTextInputTarget target = std::move(target_);
            const QString text_to_inject = final_text;
            const bool should_inject = inject_on_complete_ && usableInjectionTarget(target);
            inject_on_complete_ = false;
            hidePopup();
            QTimer::singleShot(160, this, [this, target = std::move(target), text_to_inject, should_inject] {
                if (!should_inject) {
                    recording_control_->setToolTip(QStringLiteral("没有输入目标，已复制到剪贴板"));
                    return;
                }
                const bool injected = injectIntoTarget(target, text_to_inject);
                if (injected) {
                    last_target_ = {
                        .window = target.window,
                        .focus = target.focus,
                    };
                    recording_control_->setToolTip(QStringLiteral("已输入到光标位置"));
                } else {
                    recording_control_->setToolTip(QStringLiteral("已复制到剪贴板"));
                    showPopup();
                    setBubbleStatus(QStringLiteral("注入失败，已复制到剪贴板"));
                    QTimer::singleShot(900, this, [this] { hidePopup(); });
                }
            });
#else
            refreshTranscript();
            recording_control_->setToolTip(QStringLiteral("已复制到剪贴板"));
            QTimer::singleShot(700, this, [this] { hidePopup(); });
#endif
        }
    }

    void handleRecognition(const RecognitionEvent& event) {
        if (state_ == State::Stopping && !stop_should_commit_) return;
        if (event.kind == RecognitionEventKind::Error) {
            setTransientStatus(QStringLiteral("识别失败"));
            return;
        }
        const QString value = to_qstring(event.text).trimmed();
        if (event.kind == RecognitionEventKind::Final) {
            if (!value.isEmpty()) {
                committed_text_ += value;
            }
            partial_text_.clear();
        } else {
            partial_text_ = value;
        }
        if (state_ != State::Stopping) refreshTranscript();
    }

    void setTransientStatus(const QString& text) {
        setBubbleStatus(text);
        status_reset_timer_.stop();
        status_reset_timer_.start(1500);
    }

    void setBubbleStatus(const QString& text) {
        transcript_->setBubbleText(text);
        updateWindowGeometry();
    }

    void refreshTranscript() {
        const QString display = currentTranscript();
        if (display.isEmpty()) {
            setBubbleStatus(state_ == State::Listening
                ? QStringLiteral("正在听取...")
                : QStringLiteral("按住 %1").arg(shortcutDisplayName(hotkey_shortcut_)));
            return;
        }
        transcript_->setBubbleText(display);
        updateWindowGeometry();
    }

    QString currentTranscript() const {
        QString display = committed_text_;
        if (!partial_text_.isEmpty()) {
            display += partial_text_;
        }
        return display;
    }

    void beginStableSessionGeometry() {
        if (!geometry_anchor_valid_) {
            geometry_anchor_left_ = x();
            geometry_anchor_bottom_ = y() + height();
            geometry_anchor_valid_ = true;
        }
    }

    void updateWindowGeometry() {
        const QSize bubble_size = transcript_->size();
        const bool preserve_anchor = isVisible();
        if (preserve_anchor && !geometry_anchor_valid_) {
            geometry_anchor_left_ = x();
            geometry_anchor_bottom_ = y() + height();
            geometry_anchor_valid_ = true;
        }
        const int horizontal_anchor = geometry_anchor_valid_ ? geometry_anchor_left_ : x();
        const int bottom_anchor = geometry_anchor_valid_
            ? geometry_anchor_bottom_
            : y() + height();
        const int control_width = recording_control_ == nullptr ? 0 : recording_control_->width();
        const int new_width = std::max({bubble_size.width(), control_width, 74}) +
            window_margin * 2;
        const int control_height = recording_control_ == nullptr ? 48 : recording_control_->height();
        const int new_height = bubble_size.height() + control_height + control_spacing + window_margin * 2;
        int new_x = x();
        int new_y = y();
        if (preserve_anchor) {
            new_x = horizontal_anchor;
            new_y = bottom_anchor - new_height;
            if (QScreen* current_screen = screen()) {
                const QRect area = current_screen->availableGeometry();
                new_y = new_height <= area.height()
                    ? std::clamp(new_y, area.top(), area.bottom() - new_height + 1)
                    : area.top();
            }
        }

        const QRect target_geometry(new_x, new_y, new_width, new_height);
        if (!preserve_anchor) {
            setGeometry(target_geometry);
            return;
        }
        if (geometry() == target_geometry) return;
        if (x() != new_x) move(new_x, y());
        const QSize target_size(new_width, new_height);
        if (size_animation_.state() == QAbstractAnimation::Running) {
            size_animation_.setEndValue(target_size);
            return;
        }
        size_animation_.setStartValue(size());
        size_animation_.setEndValue(target_size);
        size_animation_.start();
    }

    void updateMeter() {
        AudioLevelMetrics audio;
        VadResult vad;
        if (microphone_ != nullptr) audio = microphone_->level_metrics();
        if (recognizer_ != nullptr) vad = recognizer_->vad_telemetry();
        recording_control_->setTelemetry(audio.input_rms_db, vad.activity);
        if (state_ == State::Listening && session_elapsed_.isValid()) {
            recording_control_->setElapsedMilliseconds(session_elapsed_.elapsed());
        }

        if (state_ != State::Listening && state_ != State::Stopping) return;
        QString activity = QStringLiteral("静音");
        if (vad.activity == VadActivity::Candidate) activity = QStringLiteral("准备");
        else if (vad.activity == VadActivity::Speech) activity = QStringLiteral("语音");
        else if (vad.activity == VadActivity::EndpointWait) activity = QStringLiteral("等待句尾");
        if (audio.clipped_percent >= 0.1F) {
            recording_control_->setToolTip(QStringLiteral("输入削波 %1% · 请降低系统麦克风音量")
                                               .arg(audio.clipped_percent, 0, 'f', 1));
        } else {
            recording_control_->setToolTip(QStringLiteral("输入 %1 dBFS · VAD %2 · 门限 %3 dBFS")
                                               .arg(audio.input_rms_db, 0, 'f', 1)
                                               .arg(activity)
                                               .arg(vad.required_db, 0, 'f', 1));
        }
    }

    QSettings settings_;
    VadSettings vad_settings_;
    TextMode text_mode_ = TextMode::Clean;
    QString hotkey_shortcut_ = QString::fromLatin1(default_hotkey);
    BubbleStyle bubble_style_ = BubbleStyle::Ring;
    bool preview_mode_ = false;
    TranscriptBubble* transcript_ = nullptr;
    RecordingControl* recording_control_ = nullptr;
    QSystemTrayIcon* tray_icon_ = nullptr;
    QMenu* history_menu_ = nullptr;
    QTimer meter_timer_;
    QTimer status_reset_timer_;
    QElapsedTimer session_elapsed_;
    QVariantAnimation size_animation_;
#ifdef _WIN32
    static constexpr int hotkey_primary_id = 0x5340;
    static constexpr int hotkey_left_id = 0x5341;
    static constexpr int hotkey_right_id = 0x5342;
    static constexpr UINT hotkey_state_message = WM_APP + 0x341;
    inline static HHOOK keyboard_hook_ = nullptr;
    inline static HWND hotkey_message_window_ = nullptr;
    QTimer hotkey_hold_timer_;
    QTimer hotkey_release_timer_;
    HotkeyBinding hotkey_binding_;
    WindowsTextInputTarget target_;
    WindowsTextInputTarget last_target_;
    bool hotkey_primary_registered_ = false;
    bool hotkey_left_registered_ = false;
    bool hotkey_right_registered_ = false;
    bool hotkey_pending_ = false;
    bool hotkey_recording_ = false;
#endif

    SenseVoiceEngine engine_;
    FsmnVadEngine vad_;
    TextProcessor text_processor_;
    std::unique_ptr<StreamRecognizer> recognizer_;
    std::unique_ptr<MicrophoneCapture> microphone_;
    std::thread loader_;
    std::thread stopper_;
    std::atomic<bool> shutting_down_{false};
    State state_ = State::Loading;
    QString committed_text_;
    QString partial_text_;
    QPoint drag_offset_;
    bool dragging_ = false;
    int geometry_anchor_left_ = 0;
    int geometry_anchor_bottom_ = 0;
    bool geometry_anchor_valid_ = false;
    bool stop_should_commit_ = false;
    bool inject_on_complete_ = false;
};

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("SenseVoice 语音输入"));
    application.setOrganizationName(QStringLiteral("SenseVoice"));
    application.setWindowIcon(sensevoiceIcon());

    const QStringList arguments = application.arguments();
#ifdef _WIN32
    const QString raw_command_line = QString::fromWCharArray(GetCommandLineW());
#else
    const QString raw_command_line;
#endif
    const bool preview_mode = arguments.contains(QStringLiteral("--preview")) ||
        raw_command_line.contains(QStringLiteral("--preview"), Qt::CaseInsensitive) ||
        qEnvironmentVariableIntValue("SENSEVOICE_PREVIEW") == 1;
    const bool preview_all = arguments.contains(QStringLiteral("--preview-all")) ||
        raw_command_line.contains(QStringLiteral("--preview-all"), Qt::CaseInsensitive);
    const bool preview_active = arguments.contains(QStringLiteral("--preview-active")) ||
        raw_command_line.contains(QStringLiteral("--preview-active"), Qt::CaseInsensitive);
    const bool preview_geometry_test = arguments.contains(QStringLiteral("--preview-geometry-test")) ||
        raw_command_line.contains(QStringLiteral("--preview-geometry-test"), Qt::CaseInsensitive);
    if (preview_mode || preview_all) {
        application.setQuitOnLastWindowClosed(false);
    }
    BubbleStyle preview_style = BubbleStyle::Ring;
    QString preview_image_path;
    QString preview_text = QStringLiteral(
        "这一句用于比较浮窗方案的文字布局。说长一点时，气泡会自动扩展，不滚动，也不会裁切内容。\n"
        "第二段会保留在同一个浮窗中，方便观察长内容的宽高变化。\n"
        "按住 Ctrl + Win 开始录音，松开后会注入到上一次定位的光标位置。");
    for (int index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == QStringLiteral("--bubble-style") && index + 1 < arguments.size()) {
            preview_style = bubbleStyleFromName(arguments[++index]);
        } else if (arguments[index] == QStringLiteral("--preview-image") && index + 1 < arguments.size()) {
            preview_image_path = arguments[++index];
        } else if (arguments[index] == QStringLiteral("--preview-text") && index + 1 < arguments.size()) {
            preview_text = arguments[++index];
        }
    }
    if (preview_style == BubbleStyle::Panel) {
        if (raw_command_line.contains(QStringLiteral("capsule"), Qt::CaseInsensitive)) {
            preview_style = BubbleStyle::Capsule;
        } else if (raw_command_line.contains(QStringLiteral("ring"), Qt::CaseInsensitive)) {
            preview_style = BubbleStyle::Ring;
        }
    }
#ifdef _WIN32
    HANDLE instance_mutex = nullptr;
    if (!preview_mode && !preview_all) {
        instance_mutex = CreateMutexW(nullptr, TRUE, L"Local\\SenseVoiceLocalDictation");
        if (instance_mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing_window = FindWindowW(nullptr, L"SenseVoice 语音输入");
            if (existing_window != nullptr) {
                ShowWindow(existing_window, SW_SHOWNOACTIVATE);
                SetWindowPos(existing_window, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            MessageBoxW(nullptr, L"SenseVoice 语音输入已经在运行。请使用已有悬浮窗，或先退出旧实例。",
                         L"SenseVoice", MB_OK | MB_ICONINFORMATION);
            if (instance_mutex != nullptr) CloseHandle(instance_mutex);
            return 0;
        }
    }
#endif

    application.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    application.setStyleSheet(QStringLiteral(
        "QToolTip { color: #FFFFFF; background: #30343A; border: none; padding: 5px; }"
        "QMenu { background: white; color: #25292E; border: 1px solid #DFE2E5; padding: 5px; }"
        "QMenu::item { min-width: 130px; padding: 7px 18px; border-radius: 4px; }"
        "QMenu::item:selected { background: #EFF8F5; color: #168E68; }"
        "QMenu::item:disabled { color: #B6BBC1; }"));

    std::vector<std::unique_ptr<VoiceInputWindow>> preview_windows;
    std::unique_ptr<VoiceInputWindow> single_window;
    if (preview_all) {
        const std::array<BubbleStyle, 3> styles = {
            BubbleStyle::Capsule, BubbleStyle::Panel, BubbleStyle::Ring};
        QScreen* screen = QGuiApplication::primaryScreen();
        const QRect area = screen == nullptr ? QRect(0, 0, 1440, 900) : screen->availableGeometry();
        int left = area.left() + 36;
        for (const BubbleStyle style : styles) {
            auto window = std::make_unique<VoiceInputWindow>(style, true);
            window->setPreviewContent(preview_text);
            window->show();
            window->move(left, area.bottom() - window->height() - 64);
            window->raise();
            left += window->width() + 24;
            preview_windows.push_back(std::move(window));
        }
    } else {
        single_window = std::make_unique<VoiceInputWindow>(preview_style, preview_mode);
        if (preview_mode) {
            single_window->setPreviewContent(preview_text);
            if (preview_active) single_window->setPreviewSignal();
            single_window->show();
            single_window->raise();
            if (preview_geometry_test) single_window->startPreviewGeometryTest();
        } else {
            single_window->hideUntilInput();
        }
    }

    if (preview_mode && !preview_image_path.isEmpty() && single_window != nullptr) {
        application.processEvents();
        const QSize snapshot_size = single_window->size().expandedTo(QSize(1, 1));
        QImage snapshot(snapshot_size, QImage::Format_ARGB32_Premultiplied);
        snapshot.fill(Qt::transparent);
        single_window->render(&snapshot);
        snapshot.save(preview_image_path);
        return 0;
    }

    const int exit_code = application.exec();
#ifdef _WIN32
    if (instance_mutex != nullptr) {
        ReleaseMutex(instance_mutex);
        CloseHandle(instance_mutex);
    }
#endif
    return exit_code;
}
