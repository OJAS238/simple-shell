// simple_shell.cpp - A basic UNIX-style command line interpreter in C++17
//
// Features
//   * Prompt showing user@host:cwd
//   * Runs any external program (fork + execvp)
//   * Built-ins: cd, pwd, exit, help, history, export
//   * Pipes            :  ls | grep cpp | wc -l
//   * I/O redirection  :  sort < in.txt > out.txt      (>> to append)
//   * Background jobs  :  sleep 10 &
//   * Quoting          :  echo "hello   world"  'single quotes'  back\ slash
//   * Variables        :  echo $HOME  $?   and  ~  expansion
//   * Ctrl+C interrupts the running command, not the shell; Ctrl+D exits
//
// Build : g++ -std=c++17 -Wall -Wextra -O2 -o myshell simple_shell.cpp
// Run   : ./myshell

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

// ----------------------------------------------------------------------------
// Data structures
// ----------------------------------------------------------------------------
struct Token {
    string text;
    bool isOperator;  // true for unquoted | < > >> &
};

struct Command {
    vector<string> args;
    string inFile;
    string outFile;
    bool append = false;
};

struct Pipeline {
    vector<Command> cmds;
    bool background = false;
};

// ----------------------------------------------------------------------------
// Global shell state
// ----------------------------------------------------------------------------
static vector<string> g_history;
static int g_lastStatus = 0;
static bool g_running = true;
static int g_exitCode = 0;
static volatile sig_atomic_t g_sigint = 0;

static void onSigint(int) {
    g_sigint = 1;
    const char nl = '\n';
    ssize_t r = write(STDOUT_FILENO, &nl, 1);
    (void)r;
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static string homeDir() {
    const char* h = getenv("HOME");
    if (h) return h;
    struct passwd* pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/";
}

static string currentDir() {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return buf;
    return "?";
}

static string buildPrompt() {
    string user = "user";
    if (struct passwd* pw = getpwuid(getuid())) user = pw->pw_name;
    char host[256] = "localhost";
    gethostname(host, sizeof(host));

    string cwd = currentDir();
    string home = homeDir();
    if (!home.empty() && cwd.compare(0, home.size(), home) == 0 &&
        (cwd.size() == home.size() || cwd[home.size()] == '/'))
        cwd = "~" + cwd.substr(home.size());

    return "\033[1;32m" + user + "@" + string(host) + "\033[0m:\033[1;34m" + cwd +
           "\033[0m$ ";
}

// ----------------------------------------------------------------------------
// Tokenizer: splits a line into words and operators, honouring quotes
// ----------------------------------------------------------------------------
static bool tokenize(const string& line, vector<Token>& out, string& err) {
    string cur;
    bool inTok = false;
    size_t i = 0;

    auto flush = [&]() {
        if (inTok) {
            out.push_back({cur, false});
            cur.clear();
            inTok = false;
        }
    };

    while (i < line.size()) {
        char c = line[i];
        if (c == ' ' || c == '\t') {
            flush();
            i++;
        } else if (c == '\'') {  // single quotes: everything literal
            inTok = true;
            i++;
            while (i < line.size() && line[i] != '\'') cur += line[i++];
            if (i >= line.size()) { err = "unterminated single quote"; return false; }
            i++;
        } else if (c == '"') {  // double quotes: allow \" and \\ escapes
            inTok = true;
            i++;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size() &&
                    (line[i + 1] == '"' || line[i + 1] == '\\'))
                    i++;
                cur += line[i++];
            }
            if (i >= line.size()) { err = "unterminated double quote"; return false; }
            i++;
        } else if (c == '\\' && i + 1 < line.size()) {
            inTok = true;
            cur += line[i + 1];
            i += 2;
        } else if (c == '|' || c == '<' || c == '&') {
            flush();
            out.push_back({string(1, c), true});
            i++;
        } else if (c == '>') {
            flush();
            if (i + 1 < line.size() && line[i + 1] == '>') {
                out.push_back({">>", true});
                i += 2;
            } else {
                out.push_back({">", true});
                i++;
            }
        } else if (c == '#' && !inTok) {  // comment
            break;
        } else {
            inTok = true;
            cur += c;
            i++;
        }
    }
    flush();
    return true;
}

// ----------------------------------------------------------------------------
// Parser: tokens -> Pipeline
// ----------------------------------------------------------------------------
static bool parse(const vector<Token>& toks, Pipeline& p, string& err) {
    Command cur;
    for (size_t i = 0; i < toks.size(); i++) {
        const Token& t = toks[i];
        if (!t.isOperator) {
            cur.args.push_back(t.text);
            continue;
        }
        if (t.text == "|") {
            if (cur.args.empty()) { err = "syntax error near '|'"; return false; }
            p.cmds.push_back(cur);
            cur = Command();
        } else if (t.text == "<" || t.text == ">" || t.text == ">>") {
            if (i + 1 >= toks.size() || toks[i + 1].isOperator) {
                err = "syntax error: expected file name after '" + t.text + "'";
                return false;
            }
            if (t.text == "<") {
                cur.inFile = toks[i + 1].text;
            } else {
                cur.outFile = toks[i + 1].text;
                cur.append = (t.text == ">>");
            }
            i++;
        } else if (t.text == "&") {
            if (i != toks.size() - 1) {
                err = "'&' is only supported at the end of a line";
                return false;
            }
            p.background = true;
        }
    }
    if (cur.args.empty()) {
        err = "syntax error: missing command";
        return false;
    }
    p.cmds.push_back(cur);
    return true;
}

// ----------------------------------------------------------------------------
// Expansion of ~, $VAR and $?
// ----------------------------------------------------------------------------
static void expand(Command& c) {
    for (string& a : c.args) {
        if (a == "~" || a.compare(0, 2, "~/") == 0) {
            a = homeDir() + a.substr(1);
        } else if (a == "$?") {
            a = to_string(g_lastStatus);
        } else if (a.size() > 1 && a[0] == '$') {
            const char* v = getenv(a.c_str() + 1);
            a = v ? v : "";
        }
    }
}

// ----------------------------------------------------------------------------
// Built-in commands
// ----------------------------------------------------------------------------
static bool isBuiltin(const string& n) {
    return n == "cd" || n == "pwd" || n == "exit" || n == "help" || n == "history" ||
           n == "export";
}

// Built-ins that must change the shell's own state, so they run in the parent
static bool isParentOnly(const string& n) { return n == "cd" || n == "exit" || n == "export"; }

static int runBuiltin(const vector<string>& a) {
    const string& cmd = a[0];

    if (cmd == "cd") {
        string target;
        if (a.size() < 2) {
            target = homeDir();
        } else if (a[1] == "-") {
            const char* old = getenv("OLDPWD");
            if (!old) { cerr << "cd: OLDPWD not set\n"; return 1; }
            target = old;
            cout << target << "\n";
        } else {
            target = a[1];
        }
        string before = currentDir();
        if (chdir(target.c_str()) != 0) {
            cerr << "cd: " << target << ": " << strerror(errno) << "\n";
            return 1;
        }
        setenv("OLDPWD", before.c_str(), 1);
        setenv("PWD", currentDir().c_str(), 1);
        return 0;
    }
    if (cmd == "pwd") {
        cout << currentDir() << endl;
        return 0;
    }
    if (cmd == "exit") {
        g_running = false;
        g_exitCode = (a.size() > 1) ? atoi(a[1].c_str()) : g_lastStatus;
        return g_exitCode;
    }
    if (cmd == "history") {
        for (size_t i = 0; i < g_history.size(); i++)
            cout << " " << (i + 1) << "  " << g_history[i] << "\n";
        cout.flush();
        return 0;
    }
    if (cmd == "export") {
        if (a.size() < 2) { cerr << "usage: export NAME=value\n"; return 1; }
        for (size_t i = 1; i < a.size(); i++) {
            size_t eq = a[i].find('=');
            if (eq == string::npos || eq == 0) {
                cerr << "export: invalid format '" << a[i] << "' (use NAME=value)\n";
                return 1;
            }
            setenv(a[i].substr(0, eq).c_str(), a[i].substr(eq + 1).c_str(), 1);
        }
        return 0;
    }
    if (cmd == "help") {
        cout << "Simple Shell - built-in commands:\n"
                "  cd [dir]        change directory (cd - goes back, cd alone goes home)\n"
                "  pwd             print current directory\n"
                "  history         list commands entered so far\n"
                "  export N=v      set an environment variable\n"
                "  help            show this message\n"
                "  exit [code]     leave the shell\n"
                "\n"
                "Also supported:\n"
                "  cmd1 | cmd2     pipes            cmd > f, cmd >> f, cmd < f   redirection\n"
                "  cmd &           run in background  $VAR, $?, ~   expansion\n"
                "  Ctrl+C          interrupt running command   Ctrl+D  exit\n";
        cout.flush();
        return 0;
    }
    return 1;
}

// ----------------------------------------------------------------------------
// Executor
// ----------------------------------------------------------------------------
[[noreturn]] static void runChild(const Command& c, int inFd, int outFd, const vector<int>& toClose) {
    // Children get default signal behaviour so Ctrl+C kills them
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);

    if (inFd != -1) dup2(inFd, STDIN_FILENO);
    if (outFd != -1) dup2(outFd, STDOUT_FILENO);
    for (int fd : toClose) close(fd);

    if (!c.inFile.empty()) {
        int f = open(c.inFile.c_str(), O_RDONLY);
        if (f < 0) { cerr << "myshell: " << c.inFile << ": " << strerror(errno) << "\n"; _exit(1); }
        dup2(f, STDIN_FILENO);
        close(f);
    }
    if (!c.outFile.empty()) {
        int flags = O_WRONLY | O_CREAT | (c.append ? O_APPEND : O_TRUNC);
        int f = open(c.outFile.c_str(), flags, 0644);
        if (f < 0) { cerr << "myshell: " << c.outFile << ": " << strerror(errno) << "\n"; _exit(1); }
        dup2(f, STDOUT_FILENO);
        close(f);
    }

    if (isBuiltin(c.args[0])) {
        int rc = runBuiltin(c.args);
        cout.flush();
        _exit(rc);
    }

    vector<char*> argv;
    for (const string& s : c.args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());

    int e = errno;
    if (e == ENOENT)
        cerr << "myshell: " << argv[0] << ": command not found\n";
    else
        cerr << "myshell: " << argv[0] << ": " << strerror(e) << "\n";
    _exit(e == ENOENT ? 127 : 126);
}

static int execute(Pipeline& p) {
    for (Command& c : p.cmds) expand(c);

    // cd / exit / export must run in the shell process itself
    if (p.cmds.size() == 1 && isParentOnly(p.cmds[0].args[0])) return runBuiltin(p.cmds[0].args);

    size_t n = p.cmds.size();
    vector<pid_t> pids;
    int prevRead = -1;

    for (size_t i = 0; i < n; i++) {
        int fd[2] = {-1, -1};
        if (i + 1 < n && pipe(fd) < 0) {
            perror("pipe");
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (fd[0] != -1) { close(fd[0]); close(fd[1]); }
            break;
        }
        if (pid == 0) {
            vector<int> toClose;
            if (prevRead != -1) toClose.push_back(prevRead);
            if (fd[0] != -1) toClose.push_back(fd[0]);
            if (fd[1] != -1) toClose.push_back(fd[1]);
            runChild(p.cmds[i], prevRead, fd[1], toClose);
        }
        pids.push_back(pid);
        if (prevRead != -1) close(prevRead);
        if (fd[1] != -1) close(fd[1]);
        prevRead = fd[0];
    }
    if (prevRead != -1) close(prevRead);

    if (pids.empty()) return 1;

    if (p.background) {
        cout << "[bg] pid " << pids.back() << endl;
        return 0;
    }

    int status = 0, last = 0;
    for (pid_t pid : pids) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (pid == pids.back()) {
            if (WIFEXITED(status))
                last = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                last = 128 + WTERMSIG(status);
        }
    }
    return last;
}

// Collect finished background jobs (only called when no foreground job runs)
static void reapBackground() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) cout << "[bg] pid " << pid << " done\n";
}

// ----------------------------------------------------------------------------
// main loop (REPL)
// ----------------------------------------------------------------------------
int main() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSigint;  // no SA_RESTART: lets Ctrl+C interrupt getline
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGQUIT, SIG_IGN);

    const bool interactive = isatty(STDIN_FILENO);
    if (interactive) cout << "Simple Shell - type 'help' for commands, 'exit' to quit.\n";

    string line;
    while (g_running) {
        reapBackground();
        if (interactive) {
            cout << buildPrompt();
            cout.flush();
        }

        if (!getline(cin, line)) {
            if (g_sigint) {  // Ctrl+C at the prompt: just show a fresh prompt
                g_sigint = 0;
                cin.clear();
                continue;
            }
            if (interactive) cout << "exit\n";
            break;  // Ctrl+D / end of input
        }
        g_sigint = 0;

        vector<Token> toks;
        string err;
        if (!tokenize(line, toks, err)) { cerr << "myshell: " << err << "\n"; g_lastStatus = 2; continue; }
        if (toks.empty()) continue;

        g_history.push_back(line);

        Pipeline p;
        if (!parse(toks, p, err)) { cerr << "myshell: " << err << "\n"; g_lastStatus = 2; continue; }

        g_lastStatus = execute(p);
    }
    return g_exitCode;
}
