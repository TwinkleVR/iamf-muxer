#include "gui_input_model.h"

#include <QString>

namespace immersive_audio_muxer::gui {
namespace {

QString RoleText(app::AudioRole role) {
    return QString::fromUtf8(app::ToString(role));
}

}  // namespace

GuiInputModel::GuiInputModel(QObject* parent) : QAbstractListModel(parent) {}

int GuiInputModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : items_.size();
}

QVariant GuiInputModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size())
        return {};
    const Item& item = items_.at(index.row());
    switch (role) {
        case PathRole:
            return QString::fromUtf8(item.media.path.u8string().c_str());
        case KindRole:
            return QString::fromUtf8(app::ToString(item.media.kind));
        case DiagnosticRole:
            return QString::fromUtf8(item.media.diagnostic.c_str());
        case AudioRoleRole:
            return RoleText(item.role);
        case AudioRoleConfirmedRole:
            return item.role_confirmed;
        case ChannelsRole:
            return item.media.streams.empty() ? 0 : item.media.streams.front().channel_count;
        case DurationRole:
            return item.media.duration_ms;
        default:
            return {};
    }
}

QHash<int, QByteArray> GuiInputModel::roleNames() const {
    return {{PathRole, "path"}, {KindRole, "kind"}, {DiagnosticRole, "diagnostic"},
            {AudioRoleRole, "audioRole"}, {AudioRoleConfirmedRole, "roleConfirmed"},
            {ChannelsRole, "channels"}, {DurationRole, "durationMs"}};
}

bool GuiInputModel::Add(const app::MediaProbeInfo& info, QString* error) {
    app::MuxProject project;
    for (const Item& item : items_) {
        std::string ignored;
        if (!project.AddInput(item.media, &ignored))
            continue;
    }
    std::string message;
    if (!project.AddInput(info, &message)) {
        if (error)
            *error = QString::fromUtf8(message.c_str());
        return false;
    }
    beginInsertRows({}, items_.size(), items_.size());
    items_.push_back({info});
    endInsertRows();
    emit countChanged();
    return true;
}

void GuiInputModel::RemoveAt(int row) {
    if (row < 0 || row >= items_.size())
        return;
    beginRemoveRows({}, row, row);
    items_.removeAt(row);
    endRemoveRows();
    emit countChanged();
}

bool GuiInputModel::AssignAudioRole(int row, app::AudioRole role, bool confirmed, QString* error) {
    if (row < 0 || row >= items_.size())
        return false;
    for (int i = 0; i < items_.size(); ++i) {
        if (i != row && items_.at(i).role == role && role != app::AudioRole::kUnset) {
            if (error)
                *error = QStringLiteral("Each audio role can be assigned only once.");
            return false;
        }
    }
    items_[row].role = role;
    items_[row].role_confirmed = confirmed;
    emit dataChanged(index(row), index(row), {AudioRoleRole, AudioRoleConfirmedRole});
    return true;
}

const app::MediaProbeInfo* GuiInputModel::InfoAt(int row) const {
    return row >= 0 && row < items_.size() ? &items_.at(row).media : nullptr;
}

app::AudioRole GuiInputModel::AudioRoleAt(int row) const {
    return row >= 0 && row < items_.size() ? items_.at(row).role : app::AudioRole::kUnset;
}

}  // namespace immersive_audio_muxer::gui
