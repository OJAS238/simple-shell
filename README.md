# Simple Shell

A basic UNIX-style command line interpreter written in C++17. It works like a small version of `bash` or the Windows command prompt: it shows a prompt, reads a command, runs it, and shows the result.

## Features

- Runs any external program (`ls`, `gcc`, `grep`, ...) using `fork()` + `execvp()` + `waitpid()`
- Built-in commands: `cd`, `pwd`, `history`, `export`, `help`, `exit`
- Pipes: `ls | grep cpp | wc -l`
- I/O redirection: `<` (input), `>` (overwrite), `>>` (append)
- Background jobs: `sleep 10 &`
- Quoting (`"..."`, `'...'`, `\`) and expansion of `$VAR`, `$?` and `~`
- `Ctrl+C` stops the running command without closing the shell; `Ctrl+D` exits
- Clear error messages and exit statuses (for example `127` for command not found)

## Requirements

- macOS, Linux, or Windows with WSL (the code uses POSIX system calls)
- A C++17 compiler (`g++` or `clang++`) and `make`

On macOS, install the compiler tools with `xcode-select --install`.
On Ubuntu/WSL, use `sudo apt install build-essential`.

## Build and Run

```bash
make          # compiles simple_shell.cpp into ./myshell
./myshell     # starts the shell
```

Without `make`:

```bash
g++ -std=c++17 -Wall -Wextra -O2 -o myshell simple_shell.cpp
./myshell
```

## Example Session

```
$ ./myshell
Simple Shell - type 'help' for commands, 'exit' to quit.
user@host:~/myshell$ pwd
/Users/user/myshell
user@host:~/myshell$ ls | wc -l
4
user@host:~/myshell$ echo test > a.txt
user@host:~/myshell$ cat a.txt
test
user@host:~/myshell$ nosuchcmd
myshell: nosuchcmd: command not found
user@host:~/myshell$ echo $?
127
user@host:~/myshell$ exit
```

## Install System-wide (optional)

```bash
sudo make install     # copies myshell to /usr/local/bin
myshell               # now works from any folder
sudo make uninstall   # removes it
```

## Run with Docker (optional)

```bash
docker build -t myshell .
docker run -it myshell
```

## How It Works

1. **Read** a line from the user and split it into tokens, handling quotes and the operators `| < > >> &`.
2. **Parse** the tokens into a pipeline of commands with their redirections.
3. **Execute**:
   - `cd`, `exit` and `export` run inside the shell process, because they must change the shell's own state.
   - Every other command runs in a child created with `fork()`. The child sets up pipes and files with `pipe()` and `dup2()`, then calls `execvp()` to become the program.
   - The shell waits with `waitpid()`, or skips waiting for background jobs started with `&`.
4. **Repeat** until the user types `exit` or presses `Ctrl+D`.

## Limitations

- No wildcard expansion (`*.txt`)
- No `;`, `&&`, `||` operators
- No job control (`fg`, `bg`, `jobs`)

## Files

| File | Purpose |
|------|---------|
| `simple_shell.cpp` | Source code |
| `Makefile` | Build, install and clean targets |
| `Dockerfile` | Container build |
| `README.md` | This file |
