/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          86Box VM manager preferences module
 *
 * Authors: cold-brewed
 *
 *          Copyright 2024 cold-brewed
 */
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFontDatabase>
#include <QStyle>
#include <QTimer>
#include <cstring>

#include "qt_mcp_server.hpp"
#include "qt_preferences.hpp"
#include "qt_vmmanager_preferences.hpp"
#include "qt_vmmanager_config.hpp"
#include "ui_qt_vmmanager_preferences.h"

#ifdef Q_OS_WINDOWS
#    include "qt_vmmanager_windarkmodefilter.hpp"
extern WindowsDarkModeFilter *vmm_dark_mode_filter;
#endif

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/plat.h>
#include <86box/version.h>
}

/* The snippet MCP clients expect: a server name mapped to the endpoint URL.
   It is generated rather than stored so it always matches the port above. */
static QString
mcpClientConfig(int port)
{
    return QStringLiteral("{\n"
                          "  \"mcpServers\": {\n"
                          "    \"86box\": {\n"
                          "      \"url\": \"http://127.0.0.1:%1/mcp\"\n"
                          "    }\n"
                          "  }\n"
                          "}\n")
        .arg(port);
}

VMManagerPreferences::
    VMManagerPreferences(QWidget *parent, bool machinesRunning, McpServer *mcp_server)
    : ui(new Ui::VMManagerPreferences)
{
    ui->setupUi(this);
    ui->dirSelectButton->setIcon(QApplication::style()->standardIcon(QStyle::SP_DirIcon));
    connect(ui->dirSelectButton, &QPushButton::clicked, this, &VMManagerPreferences::chooseDirectoryLocation);

    const auto config          = new VMManagerConfig(VMManagerConfig::ConfigType::General);
    const auto configSystemDir = QString(vmm_path_cfg);
    if (!configSystemDir.isEmpty()) {
        // Prefer this one
        ui->systemDirectory->setText(QDir::toNativeSeparators(configSystemDir));
    } else if (!QString(vmm_path).isEmpty()) {
        // If specified on command line
        ui->systemDirectory->setText(QDir::toNativeSeparators(QDir(vmm_path).path()));
    }

    if (machinesRunning) {
        ui->systemDirectory->setEnabled(false);
        ui->dirSelectButton->setEnabled(false);
        ui->pushButtonDefaultSystemDir->setEnabled(false);
        ui->dirSelectButton->setToolTip(tr("To change the system directory, stop all running machines."));
    }

    ui->comboBoxLanguage->setItemData(0, 0);
    for (int i = 1; i < Preferences::languages.length(); i++) {
        ui->comboBoxLanguage->addItem(Preferences::languages[i].second, i);
        if (i == lang_id) {
            ui->comboBoxLanguage->setCurrentIndex(ui->comboBoxLanguage->findData(i));
        }
    }
    ui->comboBoxLanguage->model()->sort(Qt::AscendingOrder);

#if EMU_BUILD_NUM != 0
    const auto configUpdateCheck = config->getStringValue("update_check").toInt();
    ui->updateCheckBox->setChecked(configUpdateCheck);
#else
    ui->updateCheckBox->setVisible(false);
#endif
    const auto useRegexSearch = config->getStringValue("regex_search").toInt();
    ui->regexSearchCheckBox->setChecked(useRegexSearch);
    const auto rememberSizePosition = config->getStringValue("window_remember").toInt();
    ui->rememberSizePositionCheckBox->setChecked(rememberSizePosition);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    const auto deleteToTrash = config->getStringValue("delete_to_trash").toInt();
    ui->deleteToTrashCheckBox->setChecked(deleteToTrash);
#else
    ui->deleteToTrashCheckBox->setVisible(false);
#endif

    ui->radioButtonSystem->setChecked(color_scheme == 0);
    ui->radioButtonLight->setChecked(color_scheme == 1);
    ui->radioButtonDark->setChecked(color_scheme == 2);

#ifndef Q_OS_WINDOWS
    ui->groupBoxColorScheme->setHidden(true);
#endif

    /* MCP server: off unless configured otherwise, since it hands the machine
       list to whatever program can reach the port. */
    ui->mcpEnabledCheckBox->setChecked(config->getStringValue("mcp_enabled") != QStringLiteral("0"));
    const int mcp_port = config->getStringValue("mcp_port").toInt();
    ui->mcpPortSpinBox->setValue(((mcp_port >= MCP_MIN_PORT) && (mcp_port <= 65535)) ? mcp_port : MCP_DEFAULT_PORT);

    const auto updateMcpControls = [this]() {
        const auto enabled = ui->mcpEnabledCheckBox->isChecked();
        ui->labelMcpPort->setEnabled(enabled);
        ui->mcpPortSpinBox->setEnabled(enabled);
        ui->mcpConfigJson->setPlainText(mcpClientConfig(ui->mcpPortSpinBox->value()));
    };
    connect(ui->mcpEnabledCheckBox, &QCheckBox::toggled, this, updateMcpControls);
    connect(ui->mcpPortSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, updateMcpControls);

    ui->mcpConfigJson->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    connect(ui->mcpCopyButton, &QPushButton::clicked, this, &VMManagerPreferences::copyMcpConfig);

    if ((mcp_server != nullptr) && mcp_server->isRunning())
        ui->mcpStatusLabel->setText(tr("Listening on %1").arg(mcp_server->url()));
    else
        ui->mcpStatusLabel->setText(tr("Not running"));

    updateMcpControls();
}

VMManagerPreferences::~VMManagerPreferences()
    = default;

// Bad copy pasta from machine add
void
VMManagerPreferences::chooseDirectoryLocation()
{
    QFileDialog::Options options = QFileDialog::ShowDirsOnly;
#ifdef Q_OS_LINUX
    options |= QFileDialog::DontUseNativeDialog;
#endif
    const auto directory = QFileDialog::getExistingDirectory(this, tr("Choose directory"), ui->systemDirectory->text(), options);
    if (!directory.isEmpty())
        ui->systemDirectory->setText(QDir::toNativeSeparators(directory));
}

void
VMManagerPreferences::on_pushButtonDefaultSystemDir_released()
{
    char temp[1024];
    plat_get_vmm_dir(temp, sizeof(temp));
    ui->systemDirectory->setText(QDir::toNativeSeparators(QDir(temp).path()));
}

void
VMManagerPreferences::on_pushButtonLanguage_released()
{
    ui->comboBoxLanguage->setCurrentIndex(0);
}

void
VMManagerPreferences::copyMcpConfig()
{
    QApplication::clipboard()->setText(ui->mcpConfigJson->toPlainText());

    /* Confirm the copy without stealing any space in the dialog. */
    ui->mcpCopyButton->setText(tr("Copied!"));
    QTimer::singleShot(1500, this, [this] {
        ui->mcpCopyButton->setText(tr("Copy"));
    });
}

void
VMManagerPreferences::accept()
{
    const auto config = new VMManagerConfig(VMManagerConfig::ConfigType::General);

    strncpy(vmm_path_cfg, QDir::cleanPath(ui->systemDirectory->text()).toUtf8().constData(), sizeof(vmm_path_cfg) - 1);
    lang_id      = ui->comboBoxLanguage->currentData().toInt();
    color_scheme = (ui->radioButtonSystem->isChecked()) ? 0 : (ui->radioButtonLight->isChecked() ? 1 : 2);
    config_save_global();

#if EMU_BUILD_NUM != 0
    config->setStringValue("update_check", ui->updateCheckBox->isChecked() ? "1" : "0");
#endif
    config->setStringValue("window_remember", ui->rememberSizePositionCheckBox->isChecked() ? "1" : "0");
    config->setStringValue("regex_search", ui->regexSearchCheckBox->isChecked() ? "1" : "0");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    config->setStringValue("delete_to_trash", ui->deleteToTrashCheckBox->isChecked() ? "1" : "0");
#endif
    config->setStringValue("mcp_enabled", ui->mcpEnabledCheckBox->isChecked() ? "1" : "0");
    config->setStringValue("mcp_port", QString::number(ui->mcpPortSpinBox->value()));

    /* Write the settings out now: the server is reconfigured as soon as this
       dialog is accepted, and it reads them back from this file. */
    config->sync();
    delete config;

    QDialog::accept();
}

void
VMManagerPreferences::reject()
{
    QDialog::reject();
}
