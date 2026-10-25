#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <array>
#include <functional>

namespace sessions {
class Project;
class Transport;

// Native LX+ launcher feedback. Only the named controller's paired ports are
// touched. All protocol packets are built here; no vendor implementation is used.
class NektarPads : public QObject {
    Q_OBJECT
  public:
    enum class Mode { Notes, Clips, Scenes };
    struct Port {
        QString id, name;
        bool enabled = true;
    };
    struct Light {
        int palette = 3;
        bool on = false;
    };
    NektarPads(Project* project, Transport* transport, QObject* parent = nullptr);
    void reloadSettings();
    void setPorts(const QVector<Port>& ports);
    void setSender(std::function<bool(const QString&, const QByteArray&)> sender);
    bool connected() const {
        return m_enabled && !m_controls.isEmpty() && !m_output.isEmpty();
    }
    QString inputPort() const {
        return m_controls;
    }
    QString outputPort() const {
        return m_output;
    }
    Mode mode() const {
        return m_mode;
    }
    int bank() const {
        return m_mode == Mode::Scenes ? m_sceneBank : m_clipBank;
    }
    void setMode(Mode mode);
    bool controlChange(const QString& source, int channel, int cc, int value);
    bool takesNote(const QString& source, int channel, int note) const;
    bool note(const QString& source, int channel, int note, int velocity, bool on);
    std::array<Light, 8> lights(qint64 milliseconds) const;
    void refreshLights(bool all = false, qint64 milliseconds = -1);
    void shutdown();

    static int padNote(int pad);
    static QByteArray paletteMessage(const std::array<int, 8>& palettes);
    static QByteArray padModeMessage(bool launcher);
    static QByteArray usbModeMessage(int mode);

  signals:
    void trackChosen(int index);
    void clipChosen(int track, int scene);
    void message(const QString& title, const QString& body);

  private:
    struct Held {
        QVector<int> tracks;
        int scene = -1;
    };
    bool send(const QByteArray& data);
    bool initialize();
    void releaseHeld();
    void restoreDevice();
    void selectBank(int bank);
    void updatePorts();
    Project* m_project;
    Transport* m_transport;
    QTimer m_timer, m_settle;
    QVector<Port> m_ports;
    std::function<bool(const QString&, const QByteArray&)> m_send;
    QString m_preferred, m_controls, m_output;
    bool m_enabled = true, m_initialized = false, m_touched = false, m_shift = false;
    bool m_buttonDown[2] = {}, m_wasMode[2] = {}, m_bankUsed[2] = {};
    bool m_navigationDown[2] = {}, m_pressed[8] = {};
    int m_clipBank = 0, m_sceneBank = 0;
    Mode m_mode = Mode::Notes;
    qint64 m_retryAt = 0;
    std::array<Light, 8> m_shown{};
    std::array<Held, 8> m_held{};
    int m_shownMode = -1;
};
} // namespace sessions
