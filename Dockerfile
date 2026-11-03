# Stage 1: build
FROM gcc:13 AS build
WORKDIR /src
COPY simple_shell.cpp .
RUN g++ -std=c++17 -O2 -Wall -Wextra -o myshell simple_shell.cpp

# Stage 2: small runtime image
FROM debian:bookworm-slim
COPY --from=build /src/myshell /usr/local/bin/myshell
CMD ["myshell"]
