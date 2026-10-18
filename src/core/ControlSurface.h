#pragma once

#include <QObject>
#include <QHash>
#include <QString>

namespace sessions {
class Project;
class Transport;
class Mapper;

// A source-scoped, application-wide profile; user MIDI maps get first refusal.
// LX49+/61+ User Preset 2 supplies GM mixer CCs without proprietary firmware commands.
class ControlSurface : public QObject {
public:
    ControlSurface(Project* project, Transport* transport, Mapper* mapper, QObject* parent = nullptr);
    void reloadSettings();
    bool accepts(const QString& source) const;
    bool controlChange(const QString& source, int channel, int cc, int value);
    bool machineControl(const QString& source, int command);
    static bool isNektarImpact(const QString& source);

private:
    bool continuous(const QString& source, int channel, int cc, const QString& target, double current, double value);
    struct Pickup { int previous = -1; bool caught = false; double applied = -1; };
    Project* m_project;
    Transport* m_transport;
    Mapper* m_mapper;
    bool m_enabled = true, m_pickup = true;
    QString m_source;
    int m_bank = 0;
    QHash<QString, Pickup> m_controls;
};
}
