/*
 * Copyright © 2022 Octopull Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alan Griffiths <alan@octopull.co.uk>
 */

#include "miriway_policy.h"
#include "miriway_commands.h"

#include <miral/append_event_filter.h>
#include <miral/command_line_option.h>
#include <miral/display_configuration_option.h>
#include <miral/external_client.h>
#include <miral/internal_client.h>
#include <miral/keymap.h>
#include <miral/runner.h>
#include <miral/set_window_management_policy.h>
#include <miral/wayland_extensions.h>
#include <miral/x11_support.h>
#include <mir/log.h>

#include <sys/wait.h>

using namespace miral;
using namespace miriway;

namespace
{
struct CommandIndex
{
    explicit CommandIndex(std::function<void(std::vector<std::string> const& command_line)> launch) :
        launch{std::move(launch)}{}

    void populate(std::vector<std::string> const& config_cmds)
    {
        for (auto const& command : config_cmds)
        {
            if (command.size() < 3 || command[1] != ':')
            {
                mir::fatal_error("Invalid command option: %s", command.c_str());
            }

            commands[std::tolower(command[0])] = ExternalClientLauncher::split_command(command.substr(2));
        }
    }

    bool try_launch(char c) const
    {
        auto const i = commands.find(std::tolower(c));
        bool const found = i != end(commands);
        if (found) launch(i->second);
        return found;
    }
private:
    std::function<void (std::vector<std::string> const& command_line)> const launch;
    std::map<char, std::vector<std::string>> commands;
};

struct ShellPids
{
    void insert(pid_t pid)
    {
        std::lock_guard lock{shell_component_mutex};
        shell_component_pids.insert(pid);
    };

    void reap()
    {
        int status;
        while (true)
        {
            auto const pid = waitpid(-1, &status, WNOHANG);
            if (pid > 0)
            {
                std::lock_guard lock{shell_component_mutex};
                shell_component_pids.erase(pid);
            }
            else
            {
                break;
            }
        }
    }

    bool is_found(pid_t pid) const
    {
        std::lock_guard lock{shell_component_mutex};
        return shell_component_pids.find(pid) != end(shell_component_pids);
    }
private:
    std::mutex mutable shell_component_mutex;
    std::set<pid_t> shell_component_pids;
};
}

int main(int argc, char const* argv[])
{
    MirRunner runner{argc, argv};

    WaylandExtensions extensions;
    for (auto const& protocol : {
        WaylandExtensions::zwlr_screencopy_manager_v1,
        WaylandExtensions::zxdg_output_manager_v1,
        "zwp_pointer_constraints_v1",
        "zwp_relative_pointer_manager_v1"})
    {
        extensions.conditionally_enable(protocol, [&](WaylandExtensions::EnableInfo const& info)
            {
                return info.user_preference().value_or(true);
            });
    }

    ShellPids shell_pids;
    // Protocols we're reserving for shell components_option
    for (auto const& protocol : {
        WaylandExtensions::zwlr_layer_shell_v1,
        WaylandExtensions::zxdg_output_manager_v1,
        WaylandExtensions::zwlr_foreign_toplevel_manager_v1,
        WaylandExtensions::zwp_virtual_keyboard_manager_v1,
        WaylandExtensions::zwp_input_method_manager_v2})
    {
        extensions.conditionally_enable(protocol, [&](WaylandExtensions::EnableInfo const& info)
            {
                pid_t const pid = pid_of(info.app());
                return shell_pids.is_found(pid) || info.user_preference().value_or(false);
            });
    }

    ExternalClientLauncher client_launcher;

    CommandLineOption components_option{
        [&](std::vector<std::string> const& apps)
            {
            for (auto const& app : apps)
            {
                shell_pids.insert(client_launcher.launch(ExternalClientLauncher::split_command(app)));
            }
            },
        "shell-component",
        "Shell component to launch on startup (may be specified multiple times)"};

    CommandIndex shell_meta{[&](auto cmd){ shell_pids.insert(client_launcher.launch(cmd)); }};
    CommandIndex shell_ctrl_alt{[&](auto cmd){ shell_pids.insert(client_launcher.launch(cmd)); }};

    CommandIndex meta{[&](auto cmd){ client_launcher.launch(cmd); }};
    CommandIndex ctrl_alt{[&](auto cmd){ client_launcher.launch(cmd); }};

    ShellCommands commands{
        runner,
        [&] (auto c) { return shell_meta.try_launch(c) || meta.try_launch(c); },
        [&] (auto c) { return shell_ctrl_alt.try_launch(c) || ctrl_alt.try_launch(c); }
    };

    return runner.run_with(
        {
            X11Support{},
            extensions,
            display_configuration_options,
            client_launcher,
            components_option,
            CommandLineOption{[&](std::vector<std::string> const& cmds) { shell_ctrl_alt.populate(cmds); },
                "shell-ctrl-alt",
                "ctrl-alt <key>:<command> shortcut with shell priviledges (may be specified multiple times)"},
            CommandLineOption{[&](std::vector<std::string> const& cmds) { shell_meta.populate(cmds); },
                "shell-meta",
                "meta <key>:<command> shortcut with shell priviledges (may be specified multiple times)"},
            CommandLineOption{[&](std::vector<std::string> const& cmds) { ctrl_alt.populate(cmds); },
                "ctrl-alt", "ctrl-alt <key>:<command> shortcut (may be specified multiple times)"},
            CommandLineOption{[&](std::vector<std::string> const& cmds) { meta.populate(cmds); },
                "meta", "meta <key>:<command> shortcut (may be specified multiple times)"},
            Keymap{},
            AppendEventFilter{[&](MirEvent const* e) { shell_pids.reap(); return commands.input_event(e); }},
            set_window_management_policy<WindowManagerPolicy>(commands)
        });
}
