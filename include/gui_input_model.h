#pragma once

#include "app/project_types.h"

#include <QAbstractListModel>

namespace immersive_audio_muxer::gui {

class GuiInputModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        PathRole = Qt::UserRole + 1,
        KindRole,
        DiagnosticRole,
        AudioRoleRole,
        AudioRoleConfirmedRole,
        ChannelsRole,
        DurationRole,
    };
    Q_ENUM(Role)

    explicit GuiInputModel(QObject* parent = nullptr);

    int count() const { return items_.size(); }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool Add(const app::MediaProbeInfo& info, QString* error);
    void RemoveAt(int row);
    bool AssignAudioRole(int row, app::AudioRole role, bool confirmed, QString* error);
    const app::MediaProbeInfo* InfoAt(int row) const;
    app::AudioRole AudioRoleAt(int row) const;

signals:
    void countChanged();

private:
    struct Item {
        app::MediaProbeInfo media;
        app::AudioRole role = app::AudioRole::kUnset;
        bool role_confirmed = false;
    };

    QVector<Item> items_;
};

}  // namespace immersive_audio_muxer::gui
