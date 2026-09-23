/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          MCP tools exposing the VM manager: the virtual machine list,
 *          virtual machine creation, per-machine configuration (86box.cfg)
 *          and the live performance counters of a running machine.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#include "qt_mcp_tools.hpp"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <memory>

#include "qt_mcp_keyboard.hpp"
#include "qt_mcp_ocr.hpp"
#include "qt_vmmanager_main.hpp"
#include "qt_vmmanager_system.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/cdrom.h>
#include <86box/fdd.h>
#include <86box/plat.h>
#include <86box/video.h>
}

/* ------------------------------------------------------------------ */
/* Configuration file access                                           */
/*                                                                     */
/* An 86box.cfg is a plain INI file: [Section] headers and `key = value`*/
/* lines. Editing it line by line, instead of reserializing it, keeps   */
/* comments, ordering and line endings exactly as the user left them.   */
/* ------------------------------------------------------------------ */

struct IniLine {
    QString section; /* Section this line belongs to; empty in the header area */
    QString key;     /* Empty for blank lines and comments */
    QString value;   /* Text after '=' */
    bool    header = false; /* True when the line opens a section */
    bool    other  = false; /* True for comments and blank lines */
};

class IniFile {
public:
    static bool load(const QString &path, IniFile &out, QString &error);
    static void parse(const QString &text, IniFile &out);

    [[nodiscard]] QString     render() const;
    bool                      save(const QString &path, QString &error) const;
    [[nodiscard]] QString     value(const QString &section, const QString &key, bool *found = nullptr) const;
    [[nodiscard]] QJsonObject sections() const;

    /* Replaces or appends one key. Returns false when the section is unknown
       and creating it was not requested. */
    bool set(const QString &section, const QString &key, const QString &new_value,
             bool create_section, QString *previous_value);

private:
    QVector<IniLine> _lines;
    QString          _eol = QStringLiteral("\n");
};

void
IniFile::parse(const QString &text, IniFile &out)
{
    out._lines.clear();
    out._eol = text.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n") : QStringLiteral("\n");

    const QStringList raw_lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));

    QString section;
    for (int i = 0; i < raw_lines.size(); i++) {
        const QString &raw_line = raw_lines.at(i);
        /* A trailing newline produces one empty element: not a line. */
        if ((i == (raw_lines.size() - 1)) && raw_line.isEmpty())
            break;

        IniLine       line;
        const QString trimmed = raw_line.trimmed();

        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QLatin1Char(';'))) {
            line.other   = true;
            line.section = section;
        } else if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
            section      = trimmed.mid(1, trimmed.size() - 2).trimmed();
            line.header  = true;
            line.section = section;
            line.key     = section;
        } else {
            const int equals = trimmed.indexOf(QLatin1Char('='));
            if (equals > 0) {
                line.section = section;
                line.key     = trimmed.left(equals).trimmed();
                line.value   = trimmed.mid(equals + 1).trimmed();
            } else {
                line.other   = true;
                line.section = section;
            }
        }

        out._lines.append(line);
    }
}

bool
IniFile::load(const QString &path, IniFile &out, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }

    const QString text = QString::fromUtf8(file.readAll());
    file.close();

    parse(text, out);
    return true;
}

QString
IniFile::render() const
{
    QString text;
    for (const IniLine &line : _lines) {
        if (line.header)
            text += QStringLiteral("[%1]").arg(line.key);
        else if (line.other)
            text += QString();
        else
            text += QStringLiteral("%1 = %2").arg(line.key, line.value);
        text += _eol;
    }
    return text;
}

bool
IniFile::save(const QString &path, QString &error) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }

    file.write(render().toUtf8());
    file.close();
    return true;
}

QString
IniFile::value(const QString &section, const QString &key, bool *found) const
{
    for (const IniLine &line : _lines) {
        if (line.other || line.header)
            continue;
        if ((line.section.compare(section, Qt::CaseInsensitive) == 0)
            && (line.key.compare(key, Qt::CaseInsensitive) == 0)) {
            if (found != nullptr)
                *found = true;
            return line.value;
        }
    }

    if (found != nullptr)
        *found = false;
    return QString();
}

QJsonObject
IniFile::sections() const
{
    QJsonObject result;
    for (const IniLine &line : _lines) {
        if (line.other || line.header)
            continue;
        QJsonObject section = result.value(line.section).toObject();
        section.insert(line.key, line.value);
        result.insert(line.section, section);
    }
    return result;
}

bool
IniFile::set(const QString &section, const QString &key, const QString &new_value,
             bool create_section, QString *previous_value)
{
    if (previous_value != nullptr)
        previous_value->clear();

    int section_start = -1;
    int section_end   = _lines.size();
    int target        = -1;

    for (int i = 0; i < _lines.size(); i++) {
        if (!_lines.at(i).header || (_lines.at(i).section.compare(section, Qt::CaseInsensitive) != 0))
            continue;

        section_start = i;
        /* The section ends where the next one starts. */
        for (int j = i + 1; j < _lines.size(); j++) {
            if (_lines.at(j).header) {
                section_end = j;
                break;
            }
        }
        break;
    }

    if (section_start >= 0) {
        for (int i = section_start + 1; i < section_end; i++) {
            const IniLine &line = _lines.at(i);
            if (line.other || line.header)
                continue;
            if (line.key.compare(key, Qt::CaseInsensitive) == 0) {
                target = i;
                if (previous_value != nullptr)
                    *previous_value = line.value;
                break;
            }
        }
    } else if (!create_section) {
        return false;
    }

    if (target >= 0) {
        _lines[target].value = new_value;
        return true;
    }

    IniLine new_line;
    new_line.section = section;
    new_line.key     = key;
    new_line.value   = new_value;

    if (section_start < 0) {
        /* Append a brand new section at the end of the file. */
        if (!_lines.isEmpty() && !( _lines.last().other && _lines.last().key.isEmpty()
                                    && _lines.last().value.isEmpty())) {
            IniLine blank;
            blank.other = true;
            _lines.append(blank);
        }

        IniLine header;
        header.section = section;
        header.key     = section;
        header.header  = true;

        _lines.append(header);
        _lines.append(new_line);
        return true;
    }

    /* Insert as the last entry of the existing section. */
    _lines.insert(section_end, new_line);
    return true;
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

static QString
vm_root(void)
{
    return QDir::cleanPath(QString::fromUtf8(vmm_path));
}

/* The VM manager keys its per-machine settings by a UUID derived from the
   machine's directory path, and the emulator keeps the very same value in
   the configuration file. */
static QString
uuid_for_dir(const QString &dir)
{
    QString path = QDir::cleanPath(dir);
    if (!path.endsWith(QLatin1Char('/')))
        path.append(QLatin1Char('/'));
    return QUuid::createUuidV5(QUuid {}, path).toString(QUuid::WithoutBraces);
}

static QString
vmm_ini_path(void)
{
    char buffer[1024] = { 0 };
    plat_get_global_config_dir(buffer, sizeof(buffer) - 1);
    return QDir::cleanPath(QString::fromUtf8(buffer) + QStringLiteral("/vmm.ini"));
}

/* Resolves a machine from either its directory name or a path. */
static QString
resolve_vm_dir(const QString &identifier, QString &error)
{
    if (identifier.isEmpty()) {
        error = QStringLiteral("No virtual machine was named. Pass \"vm\" with the machine's name.");
        return QString();
    }

    const QFileInfo info(identifier);
    if (info.isAbsolute() || identifier.contains(QLatin1Char('/')) || identifier.contains(QLatin1Char('\\'))) {
        if (info.isDir())
            return QDir::cleanPath(info.absoluteFilePath());
        if (info.isFile())
            return QDir::cleanPath(info.absolutePath());
        error = QStringLiteral("No virtual machine directory at %1").arg(identifier);
        return QString();
    }

    const QString candidate = QDir(vm_root()).absoluteFilePath(identifier);
    if (!QFileInfo(candidate).isDir()) {
        error = QStringLiteral("No virtual machine named \"%1\" under %2").arg(identifier, vm_root());
        return QString();
    }

    return QDir::cleanPath(candidate);
}

static QString
config_path_for(const QString &vm_dir)
{
    return QDir(vm_dir).absoluteFilePath(QString::fromLatin1(CONFIG_FILE));
}

static bool
write_text_file(const QString &path, const QString &text, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }

    file.write(text.toUtf8());
    file.close();
    return true;
}

static QString
read_text_file(const QString &path, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return QString();
    }

    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    return text;
}

/* ------------------------------------------------------------------ */
/* Machine discovery                                                   */
/* ------------------------------------------------------------------ */

struct MachineInfo {
    QString name;         /* Directory name, also the machine's config name */
    QString display_name; /* Name shown by the VM manager */
    QString dir;
    QString config_file;
    QString uuid;
    QString notes;
    QString timestamp;
    bool    running = false;
    QString status;
};

static QVector<MachineInfo>
collect_machines(VMManagerMain *manager)
{
    QVector<MachineInfo> machines;

    const QString root = vm_root();
    if (root.isEmpty() || !QDir(root).exists())
        return machines;

    QSettings settings(vmm_ini_path(), QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    settings.setIniCodec("UTF-8");
#endif

    const QString config_file_name = QString::fromLatin1(CONFIG_FILE);
    QDirIterator  iterator(root, QStringList { config_file_name }, QDir::Files, QDirIterator::Subdirectories);

    while (iterator.hasNext()) {
        const QString file_path = iterator.next();
        const QString relative  = QDir(root).relativeFilePath(file_path);

        /* Ignore anything below a hidden directory (trash, caches). */
        bool hidden = false;
        for (const QString &part : relative.split(QLatin1Char('/')))
            if (part.startsWith(QLatin1Char('.')))
                hidden = true;
        if (hidden)
            continue;

        /* The configuration in the machine root belongs to the manager. */
        if (relative == config_file_name)
            continue;

        MachineInfo machine;
        machine.dir          = QDir::cleanPath(QFileInfo(file_path).absolutePath());
        machine.name         = QFileInfo(machine.dir).fileName();
        machine.config_file  = QDir::cleanPath(file_path);
        machine.uuid         = uuid_for_dir(machine.dir);
        machine.display_name = machine.name;

        settings.beginGroup(machine.uuid);
        const QString display_name = settings.value(QStringLiteral("display_name")).toString();
        if (!display_name.isEmpty())
            machine.display_name = display_name;
        machine.notes     = settings.value(QStringLiteral("notes")).toString();
        machine.timestamp = settings.value(QStringLiteral("timestamp")).toString();
        settings.endGroup();

        machines.append(machine);
    }

    /* Live state comes from the manager, which owns the running processes. */
    if (manager != nullptr) {
        for (VMManagerSystem *system : manager->machineList()) {
            for (MachineInfo &machine : machines) {
                if (QDir::cleanPath(system->config_dir) != machine.dir)
                    continue;
                machine.running = system->isProcessRunning();
                machine.status  = VMManagerSystem::processStatusToString(system->getProcessStatus());
            }
        }
    }

    std::sort(machines.begin(), machines.end(), [](const MachineInfo &a, const MachineInfo &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    return machines;
}

static QJsonObject
machine_to_json(const MachineInfo &machine, bool with_paths)
{
    QJsonObject object;
    object["name"]         = machine.name;
    object["display_name"] = machine.display_name;
    object["running"]      = machine.running;
    if (!machine.status.isEmpty())
        object["status"] = machine.status;
    if (!machine.notes.trimmed().isEmpty())
        object["notes"] = machine.notes;
    if (!machine.timestamp.isEmpty())
        object["last_used"] = machine.timestamp;

    if (with_paths) {
        object["config_file"] = machine.config_file;
        object["directory"]   = machine.dir;
        object["uuid"]        = machine.uuid;
    }

    return object;
}

/* ------------------------------------------------------------------ */
/* Tools                                                               */
/* ------------------------------------------------------------------ */

McpTools86Box::McpTools86Box(VMManagerMain *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager)
{
}

void
McpTools86Box::registerTools(McpServer &server)
{
    server.addTool(McpTool {
        QStringLiteral("vm_list"),
        QStringLiteral("List virtual machines"),
        QStringLiteral("List every virtual machine known to the 86Box VM manager, including "
                       "whether it is currently running. The \"name\" field identifies a "
                       "machine for the other tools."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "include_paths", QJsonObject {
                                                       { "type", QStringLiteral("boolean") },
                                                       { "description", QStringLiteral("Include config file paths, directories and UUIDs.") } } },
                                { "only_running", QJsonObject {
                                                      { "type", QStringLiteral("boolean") },
                                                      { "description", QStringLiteral("Return only the machines that are running.") } } },
                            } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            done(toolList(arguments));
        } });

    server.addTool(McpTool {
        QStringLiteral("vm_create"),
        QStringLiteral("Create a virtual machine"),
        QStringLiteral("Create a new virtual machine directory under the VM manager's machine "
                       "path and register it with the manager. Without \"config\" or "
                       "\"copy_from\" the machine starts out with an empty configuration that "
                       "the user completes in the machine's Settings window. The machine's "
                       "UUID is set to match its new location."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "name", QJsonObject {
                                              { "type", QStringLiteral("string") },
                                              { "description", QStringLiteral("Directory name of the new machine; must not exist yet.") } } },
                                { "display_name", QJsonObject {
                                                      { "type", QStringLiteral("string") },
                                                      { "description", QStringLiteral("Name shown by the VM manager. Defaults to the directory name.") } } },
                                { "copy_from", QJsonObject {
                                                   { "type", QStringLiteral("string") },
                                                   { "description", QStringLiteral("Name of an existing machine whose configuration is copied.") } } },
                                { "config", QJsonObject {
                                                { "type", QStringLiteral("string") },
                                                { "description", QStringLiteral("Full 86box.cfg contents for the new machine.") } } },
                            } },
            { "required", QJsonArray { QStringLiteral("name") } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            done(toolCreate(arguments));
        } });

    server.addTool(McpTool {
        QStringLiteral("vm_config_get"),
        QStringLiteral("Read a machine's configuration"),
        QStringLiteral("Read the configuration (86box.cfg) of a virtual machine. Without "
                       "\"section\" the whole file is returned as nested sections; with "
                       "\"section\" and \"key\" a single value is returned."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "vm", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Machine name, or the path of its config file.") } } },
                                { "section", QJsonObject {
                                                 { "type", QStringLiteral("string") },
                                                 { "description", QStringLiteral("Configuration section, for example \"Machine\".") } } },
                                { "key", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Setting inside the section, for example \"machine\".") } } },
                                { "raw", QJsonObject {
                                            { "type", QStringLiteral("boolean") },
                                            { "description", QStringLiteral("Also return the raw file contents.") } } },
                            } },
            { "required", QJsonArray { QStringLiteral("vm") } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            done(toolConfigGet(arguments));
        } });

    server.addTool(McpTool {
        QStringLiteral("vm_config_set"),
        QStringLiteral("Change a machine's configuration"),
        QStringLiteral("Set one setting in a virtual machine's 86box.cfg. A running machine "
                       "rewrites its configuration when it exits, so changing one that is "
                       "running requires \"force\" and the change may still be lost."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "vm", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Machine name, or the path of its config file.") } } },
                                { "section", QJsonObject {
                                                 { "type", QStringLiteral("string") },
                                                 { "description", QStringLiteral("Configuration section, for example \"Machine\".") } } },
                                { "key", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Setting name, for example \"mem_size\".") } } },
                                { "value", QJsonObject {
                                              { "description", QStringLiteral("New value, written as text.") } } },
                                { "create_section", QJsonObject {
                                                        { "type", QStringLiteral("boolean") },
                                                        { "description", QStringLiteral("Create the section when it does not exist yet.") } } },
                                { "force", QJsonObject {
                                               { "type", QStringLiteral("boolean") },
                                               { "description", QStringLiteral("Allow changing a running machine's configuration.") } } },
                            } },
            { "required", QJsonArray { QStringLiteral("vm"), QStringLiteral("section"), QStringLiteral("key"), QStringLiteral("value") } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            done(toolConfigSet(arguments));
        } });

    server.addTool(McpTool {
        QStringLiteral("vm_power"),
        QStringLiteral("Control a virtual machine"),
        QStringLiteral("Start, pause, reset or shut a virtual machine down, exactly like the "
                       "VM manager's toolbar buttons. Starting a machine opens its emulator "
                       "window; \"start\" does nothing when the machine already runs."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "vm", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Machine name, or the path of its config file.") } } },
                                { "action", QJsonObject {
                                                { "type", QStringLiteral("string") },
                                                { "enum", QJsonArray { QStringLiteral("start"), QStringLiteral("pause"),
                                                                       QStringLiteral("reset"), QStringLiteral("shutdown"),
                                                                       QStringLiteral("force_shutdown"),
                                                                       QStringLiteral("ctrl_alt_del") } },
                                                { "description", QStringLiteral("What to do with the machine.") } } },
                            } },
            { "required", QJsonArray { QStringLiteral("vm"), QStringLiteral("action") } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            done(toolPower(arguments));
        } });

    server.addTool(McpTool {
        QStringLiteral("vm_screenshot"),
        QStringLiteral("Look at a machine's screen"),
        QStringLiteral("Capture what the emulated display of a running machine currently shows "
                       "and return it as an image, so it is possible to see the guest's screen "
                       "(boot progress, error messages, a desktop, a game). Nothing is written "
                       "to disk. Use monitor 2 for a dual screen machine. If you cannot see "
                       "images, use vm_screen_text instead."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "vm", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Machine name. Defaults to the only running machine.") } } },
                                { "monitor", QJsonObject {
                                                 { "type", QStringLiteral("integer") },
                                                 { "description", QStringLiteral("Monitor to capture, 1 or 2. Defaults to 1.") } } },
                            } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            toolScreenshot(arguments, done);
        } });

    server.addTool(McpTool {
        QStringLiteral("vm_screen_text"),
        QStringLiteral("Read a machine's screen as text"),
        QStringLiteral("Second step for vm_screenshot: captures a running machine's screen and "
                       "returns the text recognized on it, for agents that cannot look at images. "
                       "Also useful to quote exact text (an error message, a file listing) or to "
                       "search the screen. Text mode screens use 8x8 bitmap fonts, which no OCR "
                       "engine reads perfectly, so expect a few wrong characters."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "vm", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Machine name. Defaults to the only running machine.") } } },
                                { "monitor", QJsonObject {
                                                 { "type", QStringLiteral("integer") },
                                                 { "description", QStringLiteral("Monitor to read, 1 or 2. Defaults to 1.") } } },
                                { "engine", QJsonObject {
                                                { "type", QStringLiteral("string") },
                                                { "enum", QJsonArray { QStringLiteral("auto"),
                                                                       QStringLiteral("apple-vision"),
                                                                       QStringLiteral("tesseract") } },
                                                { "description", QStringLiteral("OCR engine. \"auto\" tries every available one.") } } },
                                { "scale", QJsonObject {
                                               { "type", QStringLiteral("integer") },
                                               { "description", QStringLiteral("Upscale the capture 1-6 times before reading. 0 picks a factor automatically.") } } },
                            } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            toolScreenText(arguments, done);
        } });

    server.addTool(McpTool {
        QStringLiteral("vm_keyboard"),
        QStringLiteral("Send keys to a machine"),
        QStringLiteral("Type text or press keys in a running machine, the way a person at the "
                       "keyboard would: answer a BIOS prompt, type a DOS command, confirm a "
                       "dialog. Take a screenshot afterwards to see the result. Keys are spaced "
                       "out, so a long text takes a while.\n\n"
                       "Text is typed on a US layout. For anything that is not a plain character "
                       "use \"keys\", which takes key names and combinations such as \"enter\", "
                       "\"esc\", \"f1\", \"up\", \"tab\", \"ctrl+c\", \"ctrl+alt+del\" or "
                       "\"shift+tab\"."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "vm", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Machine name. Defaults to the only running machine.") } } },
                                { "text", QJsonObject {
                                              { "type", QStringLiteral("string") },
                                              { "description", QStringLiteral("Text to type, up to 160 characters. A newline types Enter.") } } },
                                { "keys", QJsonObject {
                                              { "type", QStringLiteral("array") },
                                              { "items", QJsonObject { { "type", QStringLiteral("string") } } },
                                              { "description", QStringLiteral("Keys to press in order, each one a name or a combination like \"ctrl+alt+del\". Sent after any text.") } } },
                                { "delay_ms", QJsonObject {
                                                  { "type", QStringLiteral("integer") },
                                                  { "description", QStringLiteral("Delay between keystrokes in milliseconds, 5-20000. Defaults to 30; raise it for a slow guest.") } } },
                            } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            toolKeyboard(arguments, done);
        } });

    server.addTool(McpTool {
        QStringLiteral("vm_media"),
        QStringLiteral("Inspect and change a machine's floppy and CD drives"),
        QStringLiteral("action=\"list\" reports the machine's floppy and optical drives, what is "
                       "in them, the images this machine has used before, and which image files are "
                       "on disk next to it (with a note whether the size fits the drive). "
                       "action=\"load\" puts an image into a drive - a folder is mounted as a "
                       "virtual ISO in an optical drive - and action=\"eject\" takes it out. "
                       "Loading and ejecting need the machine to be running."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "action", QJsonObject {
                                                { "type", QStringLiteral("string") },
                                                { "enum", QJsonArray { QStringLiteral("list"), QStringLiteral("load"), QStringLiteral("eject") } },
                                                { "description", QStringLiteral("What to do.") } } },
                                { "vm", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Machine name. Defaults to the only running machine.") } } },
                                { "drive", QJsonObject {
                                               { "type", QStringLiteral("string") },
                                               { "description", QStringLiteral("Drive to use: \"a\" or \"b\" for a floppy drive, \"cd1\"..\"cd8\" for an optical drive. Required for load and eject.") } } },
                                { "path", QJsonObject {
                                             { "type", QStringLiteral("string") },
                                             { "description", QStringLiteral("Image file (or a folder for an optical drive). Required for load.") } } },
                                { "write_protected", QJsonObject {
                                                         { "type", QStringLiteral("boolean") },
                                                         { "description", QStringLiteral("Mount a floppy read-only.") } } },
                                { "search_path", QJsonObject {
                                                     { "type", QStringLiteral("string") },
                                                     { "description", QStringLiteral("Extra folder to look for images in when listing.") } } },
                            } },
            { "required", QJsonArray { QStringLiteral("action") } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            toolMedia(arguments, done);
        } });

    server.addTool(McpTool {
        QStringLiteral("vm_performance"),
        QStringLiteral("Read live performance statistics"),
        QStringLiteral("Read the numbers shown by a running machine's Tools > Performance "
                       "window: emulation speed as a percentage of real time, guest and real "
                       "time over the sampling window, L1/L2 cache geometry and hit rates, and "
                       "the working set of touched guest memory. The machine must be running."),
        QJsonObject {
            { "type", QStringLiteral("object") },
            { "properties", QJsonObject {
                                { "vm", QJsonObject {
                                            { "type", QStringLiteral("string") },
                                            { "description", QStringLiteral("Machine name. Defaults to the only running machine.") } } },
                            } },
        },
        [this](const QJsonObject &arguments, McpDone done) {
            toolPerformance(arguments, done);
        } });
}

McpToolResult
McpTools86Box::toolList(const QJsonObject &arguments)
{
    const bool include_paths = arguments.value("include_paths").toBool(true);
    const bool only_running  = arguments.value("only_running").toBool(false);

    const QVector<MachineInfo> machines = collect_machines(_manager);

    QJsonArray array;
    int        running_count = 0;
    for (const MachineInfo &machine : machines) {
        if (machine.running)
            running_count++;
        if (only_running && !machine.running)
            continue;
        array.append(machine_to_json(machine, include_paths));
    }

    QJsonObject result;
    result["machine_path"] = vm_root();
    result["count"]        = array.size();
    result["running"]      = running_count;
    result["machines"]     = array;
    return McpToolResult::ok(result);
}

McpToolResult
McpTools86Box::toolCreate(const QJsonObject &arguments)
{
    const QString name = arguments.value("name").toString().trimmed();
    if (name.isEmpty())
        return McpToolResult::failure(QStringLiteral("A machine \"name\" is required."));
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || (name == QStringLiteral(".")) || (name == QStringLiteral("..")))
        return McpToolResult::failure(QStringLiteral("A machine name must not contain path separators."));

    const QString root = vm_root();
    if (root.isEmpty() || !QDir(root).exists())
        return McpToolResult::failure(QStringLiteral("The VM manager's machine path is not set or does not exist."));

    const QString machine_dir = QDir(root).absoluteFilePath(name);
    if (QFileInfo::exists(machine_dir))
        return McpToolResult::failure(QStringLiteral("A machine directory already exists at %1").arg(machine_dir));

    QString  config_text  = arguments.value("config").toString();
    QString  copied_from;

    if (config_text.isEmpty()) {
        const QString source = arguments.value("copy_from").toString().trimmed();
        if (!source.isEmpty()) {
            QString       error;
            const QString source_dir = resolve_vm_dir(source, error);
            if (source_dir.isEmpty())
                return McpToolResult::failure(error);

            const QString source_config = config_path_for(source_dir);
            if (!QFileInfo::exists(source_config))
                return McpToolResult::failure(QStringLiteral("The machine \"%1\" has no configuration file.").arg(source));

            QString read_error;
            config_text = read_text_file(source_config, read_error);
            if (!read_error.isEmpty())
                return McpToolResult::failure(read_error);

            copied_from = QFileInfo(source_dir).fileName();
        }
    }

    /* An empty configuration still needs the identity the manager expects, so
       the same code path patches the UUID in for every case. */
    IniFile ini;
    IniFile::parse(config_text, ini);
    ini.set(QStringLiteral("General"), QStringLiteral("uuid"), uuid_for_dir(machine_dir), true, nullptr);
    config_text = ini.render();

    QString error;
    if (!QDir().mkpath(machine_dir))
        return McpToolResult::failure(QStringLiteral("Cannot create the machine directory %1").arg(machine_dir));

    const QString config_file = config_path_for(machine_dir);
    if (!write_text_file(config_file, config_text, error))
        return McpToolResult::failure(error);

    /* Register the machine so it shows up in the manager's list. */
    const QString uuid         = uuid_for_dir(machine_dir);
    const QString display_name = arguments.value("display_name").toString().trimmed();

    QSettings settings(vmm_ini_path(), QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    settings.setIniCodec("UTF-8");
#endif
    settings.beginGroup(uuid);
    settings.setValue(QStringLiteral("system_name"), name);
    settings.setValue(QStringLiteral("config_file"), QDir::cleanPath(config_file));
    settings.setValue(QStringLiteral("config_dir"), machine_dir);
    if (!display_name.isEmpty() && (display_name != name))
        settings.setValue(QStringLiteral("display_name"), display_name);
    settings.setValue(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    settings.endGroup();
    settings.sync();

    /* Refresh the manager's own view. */
    if (_manager != nullptr) {
        _manager->reload();
        _manager->modelDataChange();
    }

    QJsonObject result;
    result["created"]      = true;
    result["name"]         = name;
    result["display_name"] = display_name.isEmpty() ? name : display_name;
    result["directory"]    = machine_dir;
    result["config_file"]  = QDir::cleanPath(config_file);
    result["uuid"]         = uuid;
    if (!copied_from.isEmpty())
        result["copied_from"] = copied_from;
    result["note"] = QStringLiteral("The machine was not started; start it from the VM manager "
                                    "or configure it in its Settings window first.");
    return McpToolResult::ok(result);
}

McpToolResult
McpTools86Box::toolConfigGet(const QJsonObject &arguments)
{
    QString       error;
    const QString vm_dir = resolve_vm_dir(arguments.value("vm").toString(), error);
    if (vm_dir.isEmpty())
        return McpToolResult::failure(error);

    const QString config_file = config_path_for(vm_dir);
    if (!QFileInfo::exists(config_file))
        return McpToolResult::failure(QStringLiteral("No configuration file at %1").arg(config_file));

    IniFile ini;
    if (!IniFile::load(config_file, ini, error))
        return McpToolResult::failure(error);

    const QString section = arguments.value("section").toString();
    const QString key     = arguments.value("key").toString();

    QJsonObject result;
    result["name"]        = QFileInfo(vm_dir).fileName();
    result["config_file"] = QDir::cleanPath(config_file);

    if (!section.isEmpty() && !key.isEmpty()) {
        bool          found = false;
        const QString value = ini.value(section, key, &found);
        if (!found)
            return McpToolResult::failure(QStringLiteral("No setting \"%1\" in section \"%2\".").arg(key, section));
        result["section"] = section;
        result["key"]     = key;
        result["value"]   = value;
    } else if (!section.isEmpty()) {
        const QJsonObject sections = ini.sections();
        if (!sections.contains(section))
            return McpToolResult::failure(QStringLiteral("No section \"%1\".").arg(section));
        result["section"]  = section;
        result["settings"] = sections.value(section);
    } else {
        result["sections"] = ini.sections();
    }

    if (arguments.value("raw").toBool(false)) {
        QString read_error;
        result["raw"] = read_text_file(config_file, read_error);
    }

    return McpToolResult::ok(result);
}

McpToolResult
McpTools86Box::toolConfigSet(const QJsonObject &arguments)
{
    QString       error;
    const QString vm_dir = resolve_vm_dir(arguments.value("vm").toString(), error);
    if (vm_dir.isEmpty())
        return McpToolResult::failure(error);

    const QString section = arguments.value("section").toString().trimmed();
    const QString key     = arguments.value("key").toString().trimmed();
    if (section.isEmpty() || key.isEmpty())
        return McpToolResult::failure(QStringLiteral("Both \"section\" and \"key\" are required."));

    const QJsonValue value_json = arguments.value("value");
    QString          value;
    if (value_json.isString())
        value = value_json.toString();
    else if (value_json.isBool())
        value = value_json.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    else if (value_json.isDouble())
        value = QString::number(value_json.toDouble(), 'g', 15);
    else
        return McpToolResult::failure(QStringLiteral("\"value\" must be text, a number or a boolean."));

    const QString config_file = config_path_for(vm_dir);
    if (!QFileInfo::exists(config_file))
        return McpToolResult::failure(QStringLiteral("No configuration file at %1").arg(config_file));

    /* A running machine saves its own configuration on exit and would
       overwrite this change, so it needs an explicit opt-in. */
    if (!arguments.value("force").toBool(false) && (_manager != nullptr)) {
        for (VMManagerSystem *system : _manager->machineList()) {
            if ((QDir::cleanPath(system->config_dir) == QDir::cleanPath(vm_dir)) && system->isProcessRunning()) {
                return McpToolResult::failure(
                    QStringLiteral("The machine \"%1\" is running and saves its configuration when "
                                   "it exits, which would overwrite this change. Stop it first, or "
                                   "pass \"force\": true.")
                        .arg(QFileInfo(vm_dir).fileName()));
            }
        }
    }

    IniFile ini;
    if (!IniFile::load(config_file, ini, error))
        return McpToolResult::failure(error);

    QString    previous       = QString();
    const bool create_section = arguments.value("create_section").toBool(true);
    if (!ini.set(section, key, value, create_section, &previous)) {
        return McpToolResult::failure(
            QStringLiteral("Section \"%1\" does not exist; pass \"create_section\": true to create it.")
                .arg(section));
    }

    if (!ini.save(config_file, error))
        return McpToolResult::failure(error);

    QJsonObject result;
    result["changed"]     = true;
    result["name"]        = QFileInfo(vm_dir).fileName();
    result["config_file"] = QDir::cleanPath(config_file);
    result["section"]     = section;
    result["key"]         = key;
    result["value"]       = value;
    result["old_value"]   = previous;
    return McpToolResult::ok(result);
}

McpToolResult
McpTools86Box::toolPower(const QJsonObject &arguments)
{
    if (_manager == nullptr)
        return McpToolResult::failure(QStringLiteral("The VM manager is not available in this process."));

    const QString name   = arguments.value("vm").toString().trimmed();
    const QString action = arguments.value("action").toString().trimmed().toLower();

    if (name.isEmpty())
        return McpToolResult::failure(QStringLiteral("A machine \"vm\" is required."));

    VMManagerSystem *target = nullptr;
    for (VMManagerSystem *system : _manager->machineList()) {
        if ((system->config_name == name) || (system->displayName == name)) {
            target = system;
            break;
        }
    }

    if (target == nullptr)
        return McpToolResult::failure(QStringLiteral("No virtual machine named \"%1\".").arg(name));

    const bool running = target->isProcessRunning();

    if (action == QStringLiteral("start")) {
        if (running)
            return McpToolResult::failure(QStringLiteral("The machine \"%1\" is already running.").arg(name));
        if (!target->canLaunch())
            return McpToolResult::failure(QStringLiteral("The 86Box binary for \"%1\" could not be located.").arg(name));
        target->startButtonPressed();
    } else if (action == QStringLiteral("pause")) {
        if (!running)
            return McpToolResult::failure(QStringLiteral("The machine \"%1\" is not running.").arg(name));
        target->pauseButtonPressed();
    } else if (action == QStringLiteral("reset")) {
        if (!running)
            return McpToolResult::failure(QStringLiteral("The machine \"%1\" is not running.").arg(name));
        target->restartButtonPressed();
    } else if (action == QStringLiteral("shutdown")) {
        if (!running)
            return McpToolResult::failure(QStringLiteral("The machine \"%1\" is not running.").arg(name));
        target->shutdownRequestButtonPressed();
    } else if (action == QStringLiteral("force_shutdown")) {
        if (!running)
            return McpToolResult::failure(QStringLiteral("The machine \"%1\" is not running.").arg(name));
        target->shutdownForceButtonPressed();
    } else if (action == QStringLiteral("ctrl_alt_del")) {
        if (!running)
            return McpToolResult::failure(QStringLiteral("The machine \"%1\" is not running.").arg(name));
        target->cadButtonPressed();
    } else {
        return McpToolResult::failure(
            QStringLiteral("Unknown action \"%1\"; use start, pause, reset, shutdown, force_shutdown or ctrl_alt_del.")
                .arg(action));
    }

    QJsonObject result;
    result["name"]         = target->config_name;
    result["display_name"] = target->displayName;
    result["action"]       = action;
    result["running"]      = target->isProcessRunning();
    result["status"]       = VMManagerSystem::processStatusToString(target->getProcessStatus());
    /* The emulator reports its state only once it has connected, so a machine
       that was just started still looks stopped for a moment. */
    if ((action == QStringLiteral("start")) && target->isProcessRunning()
        && (target->getProcessStatus() == VMManagerSystem::ProcessStatus::Stopped)) {
        result["status"] = QStringLiteral("Starting");
    }
    return McpToolResult::ok(result);
}

VMManagerSystem *
McpTools86Box::findRunningMachine(const QString &requested, QString &error)
{
    if (_manager == nullptr) {
        error = QStringLiteral("The VM manager is not available in this process.");
        return nullptr;
    }

    VMManagerSystem *target        = nullptr;
    int              running_count = 0;

    for (VMManagerSystem *system : _manager->machineList()) {
        if (!system->isProcessRunning())
            continue;
        running_count++;
        if (requested.isEmpty() || (system->config_name == requested) || (system->displayName == requested))
            target = system;
    }

    if (target != nullptr)
        return target;

    if (!requested.isEmpty()) {
        error = QStringLiteral("The machine \"%1\" is not running. Start it from the VM manager first.")
                    .arg(requested);
    } else if (running_count == 0) {
        error = QStringLiteral("No virtual machine is running.");
    } else {
        error = QStringLiteral("Several machines are running; pass \"vm\" to choose one.");
    }
    return nullptr;
}

void
McpTools86Box::requestCapture(const QJsonObject &arguments,
                              std::function<void(const ScreenRequest &request, const QByteArray &png,
                                                 const QString &error)> done)
{
    ScreenRequest request;
    request.machine      = arguments.value("vm").toString().trimmed();
    request.monitor      = arguments.value("monitor").toInt(1);
    if ((request.monitor < 1) || (request.monitor > MONITORS_NUM))
        request.monitor = 1;

    QString          error;
    VMManagerSystem *target = findRunningMachine(request.machine, error);
    if (target == nullptr) {
        done(request, QByteArray(), error);
        return;
    }

    request.machine      = target->config_name;
    request.display_name = target->displayName;

    const quint64 request_id = ++_next_request_id;
    auto          connection = std::make_shared<QMetaObject::Connection>();

    *connection = QObject::connect(target, &VMManagerSystem::screenshotReceived, this,
                                   [connection, request_id, request, done](quint64 received_id, const QJsonObject &screenshot) {
                                       if (received_id != request_id)
                                           return;
                                       QObject::disconnect(*connection);

                                       if (screenshot.contains(QStringLiteral("error"))) {
                                           done(request, QByteArray(),
                                                QStringLiteral("The machine \"%1\" could not be captured: %2")
                                                    .arg(request.machine,
                                                         screenshot.value(QStringLiteral("error")).toString()));
                                           return;
                                       }

                                       const QByteArray png = QByteArray::fromBase64(
                                           screenshot.value(QStringLiteral("data")).toString().toLatin1());
                                       if (png.isEmpty()) {
                                           done(request, QByteArray(),
                                                QStringLiteral("The machine \"%1\" returned an empty capture.")
                                                    .arg(request.machine));
                                           return;
                                       }

                                       done(request, png, QString());
                                   });

    target->requestScreenshot(request_id, request.monitor);
}

void
McpTools86Box::toolScreenshot(const QJsonObject &arguments, McpDone done)
{
    requestCapture(arguments, [done](const ScreenRequest &request, const QByteArray &png, const QString &error) {
        if (!error.isEmpty()) {
            done(McpToolResult::failure(error));
            return;
        }

        QJsonObject details;
        details["name"]         = request.machine;
        details["display_name"] = request.display_name;
        details["monitor"]      = request.monitor;
        details["format"]       = QStringLiteral("png");
        details["bytes"]        = png.size();
        /* The image block carries the picture; this is only its description. */
        details["note"] = QStringLiteral("If you cannot see images, call vm_screen_text for the same screen as text.");

        done(McpToolResult::withImage(png, QStringLiteral("image/png"), details));
    });
}

void
McpTools86Box::toolScreenText(const QJsonObject &arguments, McpDone done)
{
    const QString engine = arguments.value("engine").toString();
    const int     scale  = arguments.value("scale").toInt(0);

    requestCapture(arguments, [engine, scale, done](const ScreenRequest &request, const QByteArray &png,
                                                    const QString &error) {
        if (!error.isEmpty()) {
            done(McpToolResult::failure(error));
            return;
        }

        const McpOcr::Result ocr = McpOcr::recognize(png, engine, scale);
        if (!ocr.ok()) {
            done(McpToolResult::failure(QStringLiteral("The screen of \"%1\" could not be read: %2")
                                            .arg(request.machine, ocr.error)));
            return;
        }

        QJsonObject details;
        details["name"]         = request.machine;
        details["display_name"] = request.display_name;
        details["monitor"]      = request.monitor;
        details["width"]        = ocr.width;
        details["height"]       = ocr.height;
        details["scale"]        = ocr.scale;
        details["engine"]       = ocr.engine;
        details["text"]         = ocr.text;

        int characters = 0;
        for (const QChar &character : ocr.text) {
            if (!character.isSpace())
                characters++;
        }
        details["characters"] = characters;
        if (characters == 0)
            details["note"] = QStringLiteral("No text was recognized; the screen may show graphics only.");

        if (!ocr.lines.isEmpty()) {
            QJsonArray lines;
            for (const McpOcr::Line &line : ocr.lines) {
                QJsonObject entry;
                entry["text"] = line.text;
                if (line.confidence >= 0.0)
                    entry["confidence"] = line.confidence;
                entry["x"]      = line.x;
                entry["y"]      = line.y;
                entry["width"]  = line.width;
                entry["height"] = line.height;
                lines.append(entry);
            }
            details["lines"] = lines;
        }

        if (!ocr.alternative_text.isEmpty()) {
            details["alternative_engine"] = ocr.alternative_engine;
            details["alternative_text"]   = ocr.alternative_text;
        }

        done(McpToolResult::ok(details));
    });
}

namespace {
/* Drive naming and the defaults the emulator applies when a key is absent. */
QString floppyDriveName(int index)
{
    if (index == 1)
        return QStringLiteral("a");
    if (index == 2)
        return QStringLiteral("b");
    return QStringLiteral("floppy%1").arg(index);
}

QString floppyPrefixForName(const QString &drive_name)
{
    if (drive_name == QStringLiteral("a"))
        return QStringLiteral("fdd_01");
    if (drive_name == QStringLiteral("b"))
        return QStringLiteral("fdd_02");
    return QStringLiteral("fdd_%1").arg(drive_name.mid(6).toInt(), 2, 10, QLatin1Char('0'));
}

QString floppyDefaultType(int index)
{
    /* Both A: and B: are 5.25" 360k unless told otherwise; the extra drives
       are absent by default. */
    return (index <= 2) ? QStringLiteral("525_2dd") : QStringLiteral("none");
}

/* Media image files the emulator can open, by drive kind. */
const QStringList FLOPPY_SUFFIXES = { QStringLiteral("img"), QStringLiteral("ima"), QStringLiteral("dsk"),
                                      QStringLiteral("fdd"), QStringLiteral("86f"), QStringLiteral("imd"),
                                      QStringLiteral("td0"), QStringLiteral("fdi"), QStringLiteral("xdf"),
                                      QStringLiteral("hfe"), QStringLiteral("mfm"), QStringLiteral("raw"),
                                      QStringLiteral("do"),  QStringLiteral("d86") };

const QStringList OPTICAL_SUFFIXES = { QStringLiteral("iso"), QStringLiteral("cue"), QStringLiteral("toc"),
                                       QStringLiteral("ccd"), QStringLiteral("mds"), QStringLiteral("mdx"),
                                       QStringLiteral("chd"), QStringLiteral("aaruf"), QStringLiteral("aaruformat"),
                                       QStringLiteral("aif"), QStringLiteral("bin") };

/* "3.5\" 1.44M" -> 1474560. The drive table only carries a name, and that name
   is what tells a user which images fit. */
qint64 capacityOf(const QString &type_name)
{
    static const QRegularExpression size(QStringLiteral("([0-9]+(?:\\.[0-9]+)?)\\s*([kKmM])"));
    const auto                     match = size.match(type_name);
    if (!match.hasMatch())
        return 0;

    /* A floppy's "M" counts thousands of kilobytes: 1.44M is 1440 KB, which is
       1474560 bytes, not 1.44 * 1024 KB. */
    const double value = match.captured(1).toDouble();
    const double scale = (match.captured(2).toLower() == QStringLiteral("m")) ? (1000.0 * 1024.0) : 1024.0;
    return static_cast<qint64>(value * scale);
}

QString floppyTypeName(const QString &internal_name)
{
    QByteArray  name = internal_name.toLatin1();
    const int   type = fdd_get_from_internal_name(name.data());
    if (type <= 0)
        return internal_name;
    return QString::fromUtf8(fdd_getname(type));
}

QString opticalTypeName(const QString &internal_name)
{
    QByteArray name = internal_name.toLatin1();
    const int  type = cdrom_get_from_internal_name(name.constData());
    if (type < 0)
        return internal_name;

    char buffer[256] = { 0 };
    cdrom_get_name(type, buffer);
    return QString::fromUtf8(buffer);
}

/* The images a drive used before, newest first, as the media menu lists them. */
QStringList imageHistory(const IniFile &ini, const QString &prefix, int count)
{
    QStringList history;
    for (int i = 1; i <= count; i++) {
        const QString path = ini.value(QStringLiteral("Floppy and CD-ROM drives"),
                                       QStringLiteral("%1_image_history_%2").arg(prefix).arg(i, 2, 10, QLatin1Char('0')));
        if (!path.isEmpty())
            history.append(path);
    }
    return history;
}

QJsonArray candidatesFor(const QStringList &roots, const QStringList &suffixes, qint64 capacity,
                         bool allow_folders)
{
    QJsonArray  found;
    QStringList seen;
    int         scanned = 0;

    for (const QString &root : roots) {
        QDir directory(root);
        if (!directory.exists())
            continue;

        const QFileInfoList entries = directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                              QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (++scanned > 400)
                break;

            const QString path = QDir::cleanPath(entry.absoluteFilePath());
            if (seen.contains(path))
                continue;

            if (entry.isDir()) {
                /* Only the machine's own conventional media folders, otherwise
                   every sub-directory would be offered as a disc. */
                if (!allow_folders)
                    continue;
                if (!QStringList { QStringLiteral("media"), QStringLiteral("cdrom"), QStringLiteral("cd"),
                                   QStringLiteral("iso"), QStringLiteral("disc"), QStringLiteral("disks") }
                         .contains(entry.fileName().toLower()))
                    continue;

                seen.append(path);
                QJsonObject object;
                object["path"]        = path;
                object["folder"]      = true;
                object["mountable_as"] = QStringLiteral("virtual ISO");
                found.append(object);
                continue;
            }

            if (!suffixes.contains(entry.suffix().toLower()))
                continue;

            seen.append(path);
            QJsonObject object;
            object["path"]    = path;
            object["size_kb"] = static_cast<double>(entry.size()) / 1024.0;
            object["folder"]  = false;
            if (capacity > 0) {
                const qint64 difference = qAbs(entry.size() - capacity);
                object["fits_drive"]    = (difference <= (capacity / 50)); /* 2% slack */
            }
            if (found.size() >= 40)
                break;
            found.append(object);
        }

        if (found.size() >= 40)
            break;
    }

    return found;
}
} /* namespace */

void
McpTools86Box::toolMedia(const QJsonObject &arguments, McpDone done)
{
    const QString action = arguments.value("action").toString().trimmed().toLower();

    /* Which machine: an explicit name may be stopped, a defaulted one has to
       be the machine that is running. */
    QString          machine = arguments.value("vm").toString().trimmed();
    VMManagerSystem *target  = nullptr;

    if (machine.isEmpty()) {
        QString error;
        target = findRunningMachine(QString(), error);
        if (target == nullptr) {
            done(McpToolResult::failure(error + QStringLiteral(" Pass \"vm\" to name a machine that is not running.")));
            return;
        }
        machine = target->config_name;
    } else if (_manager != nullptr) {
        for (VMManagerSystem *system : _manager->machineList()) {
            if ((system->config_name == machine) || (system->displayName == machine)) {
                target  = system;
                machine = system->config_name;
                break;
            }
        }
    }

    QString       error;
    const QString vm_dir = resolve_vm_dir(machine, error);
    if (vm_dir.isEmpty()) {
        done(McpToolResult::failure(error));
        return;
    }

    const QString config_file = config_path_for(vm_dir);
    IniFile       ini;
    if (!IniFile::load(config_file, ini, error)) {
        done(McpToolResult::failure(error));
        return;
    }

    const QString section = QStringLiteral("Floppy and CD-ROM drives");

    if (action == QStringLiteral("list")) {
        /* Configuration files leave out drive types that equal the built-in
           default (both A: and B: default to 360k) and they list optical slots
           that are not attached, so the file is only the fallback: a running
           machine reports the drives it really has. */
        QStringList floppy_prefixes;
        QStringList optical_prefixes;

        for (int i = 1; i <= FDD_NUM; i++) {
            const QString prefix = QStringLiteral("fdd_%1").arg(i, 2, 10, QLatin1Char('0'));
            QString       type   = ini.value(section, prefix + QStringLiteral("_type"));
            if (type.isEmpty())
                type = floppyDefaultType(i);
            if (type != QStringLiteral("none"))
                floppy_prefixes.append(prefix);
        }

        for (int i = 1; i <= CDROM_NUM; i++) {
            const QString prefix = QStringLiteral("cdrom_%1").arg(i, 2, 10, QLatin1Char('0'));
            const QString type   = ini.value(section, prefix + QStringLiteral("_type"));

            /* "86cd" is the built-in default of every optical slot, so a slot
               counts as used only when something was configured or recorded. */
            const bool configured = !type.isEmpty() && (type != QStringLiteral("none"))
                                    && (type != QStringLiteral("86cd"));
            const bool recorded = !ini.value(section, prefix + QStringLiteral("_parameters")).isEmpty()
                                  || !ini.value(section, prefix + QStringLiteral("_image_path")).isEmpty()
                                  || !ini.value(section, prefix + QStringLiteral("_image_history_01")).isEmpty();
            if (configured || recorded)
                optical_prefixes.append(prefix);
        }

        /* Where to look for images: the machine's own folder with its usual
           media sub-folders, a shared media folder next to the machines, and
           wherever this machine's images live. */
        QStringList roots;
        const auto  addRoots = [&roots](const QString &base) {
            if (base.isEmpty() || roots.contains(base))
                return;
            roots.append(base);
            for (const QString &sub : QStringList { QStringLiteral("media"), QStringLiteral("floppies"),
                                                    QStringLiteral("floppy"), QStringLiteral("images"),
                                                    QStringLiteral("disks"), QStringLiteral("cdrom"),
                                                    QStringLiteral("cd"), QStringLiteral("iso") })
                roots.append(QDir(base).absoluteFilePath(sub));
        };

        addRoots(vm_dir);
        addRoots(vm_root());

        /* Also look where this machine already keeps media, whichever drive
           that media belongs to: a disc image folder often holds the floppies
           of the same product. */
        for (const QString &prefix : floppy_prefixes + optical_prefixes) {
            const QStringList paths = QStringList { ini.value(section, prefix + QStringLiteral("_fn")),
                                                    ini.value(section, prefix + QStringLiteral("_image_path")) }
                                      + imageHistory(ini, prefix, FLOPPY_IMAGE_HISTORY);
            for (const QString &path : paths) {
                QString clean = path;
                if (clean.startsWith(QStringLiteral("wp://")))
                    clean.remove(0, 5);
                if (!clean.isEmpty())
                    addRoots(QFileInfo(clean).absolutePath());
            }
        }

        const QString extra = arguments.value("search_path").toString().trimmed();
        if (!extra.isEmpty())
            addRoots(extra);

        const quint64 request_id = ++_next_request_id;
        const QString name       = QFileInfo(vm_dir).fileName();

        /* Builds one drive entry; `live` is empty when the machine is not
           running and the configuration is all there is. */
        /* Copied, not referenced: the answer may be built after this function
           has returned, once the running machine has reported its drives. */
        const auto describe = [ini, section, roots](const QString &prefix, const QJsonObject &live) {
            const bool    optical = prefix.startsWith(QStringLiteral("cdrom_"));
            const int     index   = prefix.mid(optical ? 6 : 4, 2).toInt();

            QJsonObject drive;
            drive["drive"] = optical ? QStringLiteral("cd%1").arg(index) : floppyDriveName(index);

            if (optical) {
                const QString type = ini.value(section, prefix + QStringLiteral("_type"));
                if (!type.isEmpty() && (type != QStringLiteral("none")))
                    drive["type"] = opticalTypeName(type);
                if (live.isEmpty() && !ini.value(section, prefix + QStringLiteral("_parameters")).isEmpty())
                    drive["parameters"] = ini.value(section, prefix + QStringLiteral("_parameters"));

                QString image = live.isEmpty() ? ini.value(section, prefix + QStringLiteral("_image_path"))
                                               : live.value(QStringLiteral("path")).toString();
                const bool empty = live.isEmpty() ? image.isEmpty()
                                                  : live.value(QStringLiteral("empty")).toBool(true);
                drive["empty"] = empty;
                if (!empty && !image.isEmpty()) {
                    drive["image"]  = image;
                    drive["folder"] = QFileInfo(image).isDir();
                }
            } else {
                QString type = live.isEmpty() ? ini.value(section, prefix + QStringLiteral("_type"))
                                              : live.value(QStringLiteral("type")).toString();
                if (type.isEmpty() || (type == QStringLiteral("none")))
                    type = floppyDefaultType(index);

                const QString type_name = floppyTypeName(type);
                const qint64  capacity  = capacityOf(type_name);
                drive["type"]        = type_name;
                drive["capacity_kb"] = (capacity > 0) ? (static_cast<double>(capacity) / 1024.0) : 0.0;

                QString image = live.isEmpty() ? ini.value(section, prefix + QStringLiteral("_fn"))
                                               : live.value(QStringLiteral("path")).toString();
                if (image.startsWith(QStringLiteral("wp://")))
                    image.remove(0, 5);

                const bool empty = live.isEmpty() ? image.isEmpty()
                                                  : live.value(QStringLiteral("empty")).toBool(true);
                drive["empty"] = empty;
                if (!empty && !image.isEmpty())
                    drive["image"] = image;

                const bool write_protected = live.isEmpty()
                                                 ? (ini.value(section, prefix + QStringLiteral("_writeprot")) == QStringLiteral("1"))
                                                 : live.value(QStringLiteral("write_protected")).toBool(false);
                if (write_protected)
                    drive["write_protected"] = true;
            }

            const int history_slots = optical ? CD_IMAGE_HISTORY : FLOPPY_IMAGE_HISTORY;
            const QStringList history = imageHistory(ini, prefix, history_slots);
            if (!history.isEmpty())
                drive["recent_images"] = QJsonArray::fromStringList(history);

            QStringList drive_roots = roots;
            for (const QString &used : history)
                drive_roots.append(QFileInfo(used).absolutePath());
            if (drive.contains(QStringLiteral("image")))
                drive_roots.prepend(QFileInfo(drive.value(QStringLiteral("image")).toString()).absolutePath());

            const QJsonArray candidates = candidatesFor(drive_roots,
                                                        optical ? OPTICAL_SUFFIXES : FLOPPY_SUFFIXES,
                                                        optical ? 0 : capacityOf(floppyTypeName(
                                                                      drive.value(QStringLiteral("type")).toString())),
                                                        optical);
            if (!candidates.isEmpty())
                drive["candidates"] = candidates;

            return drive;
        };

        const auto build = [floppy_prefixes, optical_prefixes, describe, target, name, vm_dir, config_file](McpDone done,
                                                                                                            const QJsonObject &live) {
            QJsonArray floppies;
            QJsonArray opticals;

            const QJsonArray live_floppies  = live.value(QStringLiteral("floppy")).toArray();
            const QJsonArray live_opticals  = live.value(QStringLiteral("optical")).toArray();

            if (!live_floppies.isEmpty()) {
                for (const auto &value : live_floppies) {
                    const QJsonObject entry = value.toObject();
                    floppies.append(describe(floppyPrefixForName(entry.value(QStringLiteral("drive")).toString()), entry));
                }
            } else {
                for (const QString &prefix : floppy_prefixes)
                    floppies.append(describe(prefix, QJsonObject()));
            }

            if (!live_opticals.isEmpty()) {
                for (const auto &value : live_opticals) {
                    const QJsonObject entry = value.toObject();
                    const int         index = entry.value(QStringLiteral("drive")).toString().mid(2).toInt();
                    opticals.append(describe(QStringLiteral("cdrom_%1").arg(index, 2, 10, QLatin1Char('0')), entry));
                }
            } else {
                for (const QString &prefix : optical_prefixes)
                    opticals.append(describe(prefix, QJsonObject()));
            }

            QJsonObject result;
            result["name"]         = name;
            result["display_name"] = (target != nullptr) ? target->displayName : name;
            result["running"]      = (target != nullptr) && target->isProcessRunning();
            result["config_file"]  = QDir::cleanPath(config_file);
            result["floppy"]       = floppies;
            result["optical"]      = opticals;
            result["note"]         = QStringLiteral("Use vm_media action=\"load\" with a \"drive\" and a \"path\" "
                                                    "to put an image or folder into a drive.");
            done(McpToolResult::ok(result));
        };

        if ((target == nullptr) || !target->isProcessRunning()) {
            build(done, QJsonObject());
            return;
        }

        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = QObject::connect(target, &VMManagerSystem::mediaActionResultReceived, this,
                                       [connection, request_id, build, done](quint64 received_id, const QJsonObject &result) {
                                           if (received_id != request_id)
                                               return;
                                           QObject::disconnect(*connection);
                                           build(done, result.value(QStringLiteral("drives")).toObject());
                                       });

        QJsonObject request;
        request["action"] = QStringLiteral("state");
        target->requestMediaAction(request_id, request);
        return;
    }

    if ((action != QStringLiteral("load")) && (action != QStringLiteral("eject"))) {
        done(McpToolResult::failure(QStringLiteral("Unknown action \"%1\": use list, load or eject.").arg(action)));
        return;
    }

    if (target == nullptr) {
        done(McpToolResult::failure(QStringLiteral("No virtual machine named \"%1\".").arg(machine)));
        return;
    }

    if (!target->isProcessRunning()) {
        done(McpToolResult::failure(
            QStringLiteral("The machine \"%1\" is not running. Media can only be changed while it is "
                           "running; for a stopped machine, set the image in its configuration with "
                           "vm_config_set instead.")
                .arg(machine)));
        return;
    }

    QJsonObject request;
    request["action"] = action;
    request["drive"]  = arguments.value("drive").toString();
    request["path"]   = arguments.value("path").toString();
    if (arguments.contains(QStringLiteral("write_protected")))
        request["write_protected"] = arguments.value(QStringLiteral("write_protected")).toBool(false);

    const quint64 request_id = ++_next_request_id;
    const QString name       = target->config_name;

    auto connection = std::make_shared<QMetaObject::Connection>();

    *connection = QObject::connect(target, &VMManagerSystem::mediaActionResultReceived, this,
                                   [connection, request_id, name, done](quint64 received_id, const QJsonObject &result) {
                                       if (received_id != request_id)
                                           return;
                                       QObject::disconnect(*connection);

                                       if (result.contains(QStringLiteral("error"))) {
                                           done(McpToolResult::failure(
                                               QStringLiteral("The machine \"%1\" refused the media change: %2")
                                                   .arg(name, result.value(QStringLiteral("error")).toString())));
                                           return;
                                       }

                                       QJsonObject details;
                                       details["name"]   = name;
                                       details["action"] = result.value(QStringLiteral("action"));
                                       details["drive"]  = result.value(QStringLiteral("drive"));
                                       details["media"]  = result.value(QStringLiteral("media"));
                                       done(McpToolResult::ok(details));
                                   });

    target->requestMediaAction(request_id, request);
}

void
McpTools86Box::toolKeyboard(const QJsonObject &arguments, McpDone done)
{
    const QString requested = arguments.value("vm").toString().trimmed();

    QString          error;
    VMManagerSystem *target = findRunningMachine(requested, error);
    if (target == nullptr) {
        done(McpToolResult::failure(error));
        return;
    }

    QJsonObject input;
    if (arguments.contains(QStringLiteral("text")))
        input["text"] = arguments.value(QStringLiteral("text")).toString();
    if (arguments.contains(QStringLiteral("keys")))
        input["keys"] = arguments.value(QStringLiteral("keys")).toArray();
    if (arguments.contains(QStringLiteral("delay_ms")))
        input["delay_ms"] = arguments.value(QStringLiteral("delay_ms")).toInt(McpKeyboard::DEFAULT_DELAY_MS);

    if (!input.contains(QStringLiteral("text")) && !input.contains(QStringLiteral("keys"))) {
        done(McpToolResult::failure(QStringLiteral("Nothing to send: pass \"text\", \"keys\", or both.")));
        return;
    }

    const quint64 request_id = ++_next_request_id;
    const QString name       = target->config_name;
    const QString display    = target->displayName;

    auto connection = std::make_shared<QMetaObject::Connection>();

    *connection = QObject::connect(target, &VMManagerSystem::keyInputResultReceived, this,
                                   [connection, request_id, name, display, done](quint64 received_id, const QJsonObject &result) {
                                       if (received_id != request_id)
                                           return;
                                       QObject::disconnect(*connection);

                                       if (result.contains(QStringLiteral("error"))) {
                                           done(McpToolResult::failure(
                                               QStringLiteral("The machine \"%1\" did not take the keys: %2")
                                                   .arg(name, result.value(QStringLiteral("error")).toString())));
                                           return;
                                       }

                                       QJsonObject details;
                                       details["name"]         = name;
                                       details["display_name"] = display;
                                       details["keystrokes"]   = result.value(QStringLiteral("keystrokes")).toInt();
                                       details["duration_ms"]  = result.value(QStringLiteral("duration_ms")).toInt();
                                       details["note"]         = QStringLiteral("Use vm_screenshot or vm_screen_text to see the result on screen.");
                                       done(McpToolResult::ok(details));
                                   });

    target->requestKeyInput(request_id, input);
}

void
McpTools86Box::toolPerformance(const QJsonObject &arguments, McpDone done)
{
    const QString requested = arguments.value("vm").toString().trimmed();

    QString          error;
    VMManagerSystem *target = findRunningMachine(requested, error);
    if (target == nullptr) {
        done(McpToolResult::failure(error));
        return;
    }

    const quint64 request_id = ++_next_request_id;
    const QString name       = target->config_name;
    const QString display    = target->displayName;

    /* The reply carries the request id, and the connection is dropped as soon
       as this call's answer has arrived. */
    auto connection = std::make_shared<QMetaObject::Connection>();

    *connection = QObject::connect(target, &VMManagerSystem::performanceStatsReceived, this,
                                   [connection, request_id, name, display, done](quint64 received_id, const QJsonObject &stats) {
                                       if (received_id != request_id)
                                           return;
                                       QObject::disconnect(*connection);

                                       if (stats.isEmpty()) {
                                           done(McpToolResult::failure(
                                               QStringLiteral("The machine \"%1\" did not report performance "
                                                              "statistics; it may have stopped.")
                                                   .arg(name)));
                                           return;
                                       }

                                       /* The emulator reports a failed sample as an error field. */
                                       if (stats.contains(QStringLiteral("error"))) {
                                           done(McpToolResult::failure(
                                               QStringLiteral("The machine \"%1\" could not be sampled: %2")
                                                   .arg(name, stats.value(QStringLiteral("error")).toString())));
                                           return;
                                       }

                                       QJsonObject result;
                                       result["name"]         = name;
                                       result["display_name"] = display;
                                       result["source"]       = QStringLiteral("Tools > Performance");
                                       result["stats"]        = stats;
                                       done(McpToolResult::ok(result));
                                   });

    target->requestPerformance(request_id);
}
