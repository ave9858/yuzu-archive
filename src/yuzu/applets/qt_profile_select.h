// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include <QDialog>
#include <QList>
#include "core/frontend/applets/profile_select.h"
#include "core/hle/service/acc/profile_manager.h"

class ControllerNavigation;
class GMainWindow;
class QDialogButtonBox;
class QGraphicsScene;
class QLabel;
class QScrollArea;
class QStandardItem;
class QStandardItemModel;
class QTreeView;
class QVBoxLayout;

namespace Core::HID {
class HIDCore;
} // namespace Core::HID

class QtProfileSelectionDialog final : public QDialog {
    Q_OBJECT

public:
    explicit QtProfileSelectionDialog(Core::HID::HIDCore& hid_core, QWidget* parent);
    ~QtProfileSelectionDialog() override;

    int exec() override;
    void accept() override;
    void reject() override;

    int GetIndex() const;

private:
    void SelectUser(const QModelIndex& index);

    int user_index = 0;

    QVBoxLayout* layout;
    QTreeView* tree_view;
    QStandardItemModel* item_model;
    QGraphicsScene* scene;

    std::vector<QList<QStandardItem*>> list_items;

    QVBoxLayout* outer_layout;
    QLabel* instruction_label;
    QScrollArea* scroll_area;
    QDialogButtonBox* buttons;

    std::unique_ptr<Service::Account::ProfileManager> profile_manager;
    ControllerNavigation* controller_navigation = nullptr;
};

class QtProfileSelector final : public QObject, public Core::Frontend::ProfileSelectApplet {
    Q_OBJECT

public:
    explicit QtProfileSelector(GMainWindow& parent);
    ~QtProfileSelector() override;

    void SelectProfile(std::function<void(std::optional<Common::UUID>)> callback_) const override;

signals:
    void MainWindowSelectProfile() const;

private:
    void MainWindowFinishedSelection(std::optional<Common::UUID> uuid);

    mutable std::function<void(std::optional<Common::UUID>)> callback;
};
