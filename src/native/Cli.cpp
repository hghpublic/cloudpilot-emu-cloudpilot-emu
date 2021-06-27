#include "Cli.h"

// clang-format off
#include <cstdio>
#include <readline/readline.h>
#include <readline/history.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "DbBackup.h"
#include "DbInstaller.h"
#include "EmCommon.h"
#include "EmErrCodes.h"
#include "EmSession.h"
#include "util.h"

namespace {
    using Task = function<bool()>;
    using Cmd = function<bool(vector<string>)>;

    atomic<bool> stop(false);
    thread cliThread;

    Task task;
    mutex dispatchMutex;
    condition_variable cvExecuteTask;

    void InstallFile(string path) {
        unique_ptr<uint8[]> buffer;
        size_t len;

        if (!util::readFile(path, buffer, len)) {
            cout << "failed to read " << path << endl << flush;
            return;
        }

        switch (DbInstaller::Install(len, buffer.get())) {
            case DbInstaller::Result::needsReboot:
                cout << "installation successful; device requires reset" << endl << flush;
                break;

            case DbInstaller::Result::success:
                cout << "installation successful" << endl << flush;
                break;

            default:
                cout << "installation failed" << endl << flush;
                break;
        }
    }

    void SaveImage(string file) {
        fstream stream(file, ios_base::out);

        if (stream.fail()) {
            cout << "failed to open " << file << endl << flush;
            return;
        }

        EmAssert(gSession);

        auto [imageSize, image] = gSession->SaveImage();

        stream.write((const char*)image.get(), imageSize);

        if (stream.fail()) {
            cout << "I/O error writing " << file << endl << flush;
        }
    }

    void SaveBackup(string file) {
        DbBackup backup;

        if (!backup.Init()) {
            cout << "backup failed" << endl << flush;

            return;
        }

        while (backup.IsInProgress()) {
            cout << "backing up " << backup.GetCurrentDatabase() << " ... ";

            if (backup.Save()) {
                cout << "success" << endl << flush;
            } else {
                cout << "failed" << endl << flush;
            }
        }

        auto [archiveSize, archive] = backup.GetArchive();

        if (archiveSize <= 0) return;

        cout << "saving backup to " << file << endl << flush;

        fstream stream(file, ios_base::out);

        if (stream.fail()) {
            cout << "failed to open " << file << endl << flush;
            return;
        }

        stream.write((const char*)archive, archiveSize);

        if (stream.fail()) {
            cout << "I/O error writing " << file << endl << flush;
        }
    }

    bool CmdQuit(vector<string> args) { return true; }

    bool CmdInstallFile(vector<string> args) {
        if (args.empty()) {
            cout << "usage: install <file> [file...]" << endl << flush;
            return false;
        }

        for (auto file : args) {
            cout << "installing '" << file << "'..." << endl << flush;
            InstallFile(file);
        }

        return false;
    }

    bool CmdSaveImage(vector<string> args) {
        if (args.size() != 1) {
            cout << "usage: save-image <file>" << endl << flush;
            return false;
        }

        cout << "saving session image to '" << args[0] << "'" << endl << flush;

        SaveImage(args[0]);

        return false;
    }

    bool CmdRandomSeed(vector<string> args) {
        if (args.size() > 0) {
            cout << "usage: random-seed" << endl << flush;
            return false;
        }

        cout << "current random seed: " << gSession->GetRandomSeed() << endl << flush;

        return false;
    }

    bool CmdSetUserName(vector<string> args) {
        if (args.size() == 0) {
            cout << "usage: set-user-name <hotsync user name>";
            return false;
        }

        string username;

        for (size_t i = 0; i < args.size(); i++) {
            username += args[i];
            if (i < args.size() - 1) username += " ";
        }

        gSession->SetHotsyncUserName(username);

        return false;
    }

    bool CmdResetSoft(vector<string> args) {
        if (args.size() > 0) {
            cout << "usage: reset-soft" << endl << flush;
            return false;
        }

        gSession->Reset(EmSession::ResetType::soft);

        return false;
    }

    bool CmdResetHard(vector<string> args) {
        if (args.size() > 0) {
            cout << "usage: reset-hard" << endl << flush;
            return false;
        }

        gSession->Reset(EmSession::ResetType::hard);

        return false;
    }

    bool CmdResetNoext(vector<string> args) {
        if (args.size() > 0) {
            cout << "usage: reset-noext" << endl << flush;
            return false;
        }

        gSession->Reset(EmSession::ResetType::noext);

        return false;
    }

    bool CmdInvalidCommand(vector<string> args) {
        cout << "invalid command" << endl << flush;
        return false;
    }

    bool CmdSwitchImage(vector<string> args) {
        if (args.size() != 1) {
            cout << "usage: switch-image <file>" << endl << flush;
            return false;
        }

        util::initializeSession(args[0]);

        return false;
    }

    bool CmdSaveBackup(vector<string> args) {
        if (args.size() != 1) {
            cout << "usage: save-backup <file>" << endl << flush;
            return false;
        }

        SaveBackup(args[0]);

        return false;
    }

    struct Command {
        string name;
        Cmd cmd;
    };

    Command commands[] = {{.name = "quit", .cmd = CmdQuit},
                          {.name = "exit", .cmd = CmdQuit},
                          {.name = "install", .cmd = CmdInstallFile},
                          {.name = "random-seed", .cmd = CmdRandomSeed},
                          {.name = "set-user-name", .cmd = CmdSetUserName},
                          {.name = "reset-soft", .cmd = CmdResetSoft},
                          {.name = "reset-hard", .cmd = CmdResetHard},
                          {.name = "reset-noext", .cmd = CmdResetNoext},
                          {.name = "save-image", .cmd = CmdSaveImage},
                          {.name = "switch-image", .cmd = CmdSwitchImage},
                          {.name = "save-backup", .cmd = CmdSaveBackup}};

    vector<string> Split(const char* line) {
        istringstream iss(line);

        return vector<string>(istream_iterator<string>(iss), istream_iterator<string>());
    }

    char* RlCompletionFunction(const char* word, int state) {
        static vector<string> words;
        if (state == 0) words = Split(rl_line_buffer);

        if (words.size() > 1 || (words.size() == 1 && strlen(word) == 0))
            return rl_filename_completion_function(word, state);

        static vector<string> suggestions;

        if (state == 0) {
            const int cmdCount = sizeof(commands) / sizeof(commands[0]);

            suggestions = vector<string>(cmdCount);
            suggestions.clear();

            for (auto& command : commands) {
                if (strstr(command.name.c_str(), word) == command.name.c_str()) {
                    suggestions.push_back(command.name);
                }
            }
        }

        if (state >= (int)suggestions.size()) return nullptr;

        char* suggestion = (char*)malloc(suggestions[state].size() + 1);
        strcpy(suggestion, suggestions[state].c_str());

        return suggestion;
    }

    int ReadlineEventHook() {
        if (stop) {
            rl_done = 1;
        }

        return 0;
    }

    void Dispatch(Task t) {
        unique_lock<mutex> lock(dispatchMutex);
        if (stop) return;

        task = t;

        while (task) cvExecuteTask.wait(lock);
    }

    void ThreadMain() {
        rl_event_hook = ReadlineEventHook;
        rl_completion_entry_function = RlCompletionFunction;

        while (!stop) {
            char* lineBuffer = readline("> ");

            if (!lineBuffer) {
                cout << endl << flush;

                Dispatch(bind(CmdQuit, vector<string>()));

                continue;
            }

            add_history(lineBuffer);

            vector<string> words = Split(lineBuffer);

            if (!words.empty() && !stop) {
                vector<string> args(words.begin() + 1, words.end());
                Cmd cmd = CmdInvalidCommand;

                for (auto& command : commands)
                    if (command.name == words[0]) {
                        cmd = command.cmd;
                        break;
                    }

                Dispatch(bind(cmd, args));
            }

            free(lineBuffer);
        }
    }
}  // namespace

namespace Cli {

    void Start() {
        if (cliThread.joinable()) return;

        stop = false;
        task = Task();

        cliThread = thread(ThreadMain);
    }

    void Stop() {
        if (!cliThread.joinable()) return;

        {
            unique_lock<mutex> lock(dispatchMutex);

            stop = true;
        }

        cvExecuteTask.notify_one();

        cliThread.join();

        task = Task();
    }

    bool Execute() {
        bool result = false;

        {
            unique_lock<mutex> lock(dispatchMutex);

            if (task) result = task();

            task = Task();
        }

        cvExecuteTask.notify_one();

        return result;
    }

}  // namespace Cli