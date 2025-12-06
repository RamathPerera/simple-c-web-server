#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <linux/limits.h>

#define PORT 2728
#define BACKLOG 10
#define BUFFER_SIZE 4096
#define METHOD_SIZE 10
#define ROUTE_SIZE 256
#define MIME_SIZE 64
#define TIMEBUF_SIZE 128

#define ROOT_DIR "htdocs"
#define ERR_DIR "htdocs/errors"
#define PHP_CGI_PATH "/usr/bin/php-cgi"

#define HTTP_OK "200 OK"
#define HTTP_BAD_REQUEST "400 Bad Request"
#define HTTP_NOT_FOUND "404 Not Found"
#define HTTP_METHOD_NOT_ALLOWED "405 Method Not Allowed"
#define HTTP_INTERNAL_ERROR "500 Internal Server Error"

void handleClient(int clientSocket);
void handleGET(int clientSocket, const char *route);

void sendResponse(int clientSocket, const char *status, const char *contentType, const char *body, long contentLength);
void sendErrorResponse(int clientSocket, const char *status);
void sendFile(int clientSocket, const char *filePath);
int executePHP(int clientSocket, const char *scriptPath, const char *queryString);
ssize_t sendAll(int socket, const void *buffer, size_t length);

void getFileURL(const char *route, char *fileURL, size_t fileURLSize);
void getMimeType(const char *file, char *mime);
void getTimeString(char *timeBuf);
int resolvePathSafe(const char *fileURL, char *resolvedPath, size_t resolvedSize);

int main()
{
    int serverSocket, clientSocket;
    struct sockaddr_in serverAddress, clientAddress;
    socklen_t addr_size = sizeof(clientAddress);

    signal(SIGCHLD, SIG_IGN);

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(PORT);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0)
    {
        perror("Bind failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    if (listen(serverSocket, BACKLOG) < 0)
    {
        perror("Listen failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on http://localhost:%d/\n", PORT);

    while (1)
    {
        clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddress, &addr_size);
        if (clientSocket < 0)
        {
            perror("Accept failed");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            perror("Fork failed");
            close(clientSocket);
        }
        else if (pid == 0)
        {
            close(serverSocket);
            handleClient(clientSocket);
            exit(0);
        }
        else
        {
            close(clientSocket);
        }
    }

    close(serverSocket);
    return 0;
}

void handleClient(int clientSocket)
{
    char request[BUFFER_SIZE];
    memset(request, 0, BUFFER_SIZE);

    ssize_t totalBytes = 0, bytesRead;
    while ((bytesRead = read(clientSocket, request + totalBytes, BUFFER_SIZE - totalBytes - 1)) > 0)
    {
        totalBytes += bytesRead;
        request[totalBytes] = '\0';
        if (strstr(request, "\r\n\r\n"))
            break;
        if (totalBytes >= BUFFER_SIZE - 1)
            break;
    }
    if (totalBytes <= 0)
    {
        close(clientSocket);
        return;
    }

    char method[METHOD_SIZE] = {0}, route[ROUTE_SIZE] = {0}, version[16] = {0};
    char *line = strtok(request, "\r\n");
    if (!line || sscanf(line, "%9s %255s %15s", method, route, version) != 3)
    {
        sendErrorResponse(clientSocket, HTTP_BAD_REQUEST);
        close(clientSocket);
        return;
    }

    if (strncmp(version, "HTTP/", 5) != 0)
    {
        sendErrorResponse(clientSocket, HTTP_BAD_REQUEST);
        close(clientSocket);
        return;
    }

    if (strstr(route, ".."))
    {
        sendErrorResponse(clientSocket, HTTP_BAD_REQUEST);
        close(clientSocket);
        return;
    }

    printf("Request: %s %s\n", method, route);

    if (strcmp(method, "GET") == 0)
    {
        handleGET(clientSocket, route);
    }
    else
    {
        sendErrorResponse(clientSocket, HTTP_METHOD_NOT_ALLOWED);
    }

    close(clientSocket);
}

void handleGET(int clientSocket, const char *route)
{
    char baseURL[ROUTE_SIZE];
    getFileURL(route, baseURL, sizeof(baseURL));

    char finalPath[PATH_MAX];
    struct stat st;

    if (stat(baseURL, &st) == 0 && S_ISDIR(st.st_mode))
    {
        snprintf(finalPath, sizeof(finalPath), "%s/index.html", baseURL);
        if (access(finalPath, F_OK) == -1)
        {
            snprintf(finalPath, sizeof(finalPath), "%s/index.php", baseURL);
            if (access(finalPath, F_OK) == -1)
            {
                sendErrorResponse(clientSocket, HTTP_NOT_FOUND);
                return;
            }
        }
    }
    else
    {
        strncpy(finalPath, baseURL, sizeof(finalPath));
    }

    char resolvedPath[PATH_MAX];
    if (!resolvePathSafe(finalPath, resolvedPath, sizeof(resolvedPath)))
    {
        sendErrorResponse(clientSocket, HTTP_NOT_FOUND);
        return;
    }

    if (stat(resolvedPath, &st) != 0 || !S_ISREG(st.st_mode))
    {
        sendErrorResponse(clientSocket, HTTP_NOT_FOUND);
        return;
    }

    const char *dot = strrchr(resolvedPath, '.');
    if (dot && strcmp(dot, ".php") == 0)
    {
        char *query = strchr(route, '?');
        char *queryString = NULL;
        if (query)
        {
            *query = '\0';
            queryString = query + 1;
        }
        if (executePHP(clientSocket, resolvedPath, queryString) != 0)
        {
            sendErrorResponse(clientSocket, HTTP_INTERNAL_ERROR);
        }
        return;
    }

    sendFile(clientSocket, resolvedPath);
}

void sendResponse(int clientSocket, const char *status, const char *contentType, const char *body, long contentLength)
{
    char header[BUFFER_SIZE];
    char timeBuf[TIMEBUF_SIZE];
    getTimeString(timeBuf);

    snprintf(header, BUFFER_SIZE,
             "HTTP/1.1 %s\r\n"
             "Date: %s\r\n"
             "Server: C-Server/1.0\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n\r\n",
             status, timeBuf, contentType, contentLength);

    sendAll(clientSocket, header, strlen(header));
    if (body)
    {
        sendAll(clientSocket, body, contentLength);
    }
}

void sendErrorResponse(int clientSocket, const char *status)
{
    char statusCode[4];
    sscanf(status, "%3s", statusCode);

    char filePath[PATH_MAX];
    snprintf(filePath, sizeof(filePath), "%s/%s.html", ERR_DIR, statusCode);

    FILE *file = fopen(filePath, "rb");
    if (file)
    {
        fseek(file, 0, SEEK_END);
        long fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);

        char *buffer = malloc(fileSize);
        if (buffer)
        {
            fread(buffer, 1, fileSize, file);
            sendResponse(clientSocket, status, "text/html", buffer, fileSize);
            free(buffer);
        }
        fclose(file);
    }
    else
    {
        char body[256];
        snprintf(body, sizeof(body), "<h1>%s</h1>", status);
        sendResponse(clientSocket, status, "text/html", body, strlen(body));
    }
}

void sendFile(int clientSocket, const char *filePath)
{
    int fileFd = open(filePath, O_RDONLY);
    if (fileFd < 0)
    {
        sendErrorResponse(clientSocket, HTTP_NOT_FOUND);
        return;
    }

    struct stat st;
    if (fstat(fileFd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        close(fileFd);
        sendErrorResponse(clientSocket, HTTP_NOT_FOUND);
        return;
    }

    char mimeType[MIME_SIZE];
    getMimeType(filePath, mimeType);

    char header[BUFFER_SIZE];
    char timeBuf[TIMEBUF_SIZE];
    getTimeString(timeBuf);
    snprintf(header, BUFFER_SIZE,
             "HTTP/1.1 %s\r\n"
             "Date: %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: close\r\n\r\n",
             HTTP_OK, timeBuf, mimeType, st.st_size);
    sendAll(clientSocket, header, strlen(header));

    off_t offset = 0;
    ssize_t sentBytes;
    while (offset < st.st_size)
    {
        sentBytes = sendfile(clientSocket, fileFd, &offset, st.st_size - offset);
        if (sentBytes <= 0)
            break;
    }

    close(fileFd);
}

int executePHP(int clientSocket, const char *scriptPath, const char *queryString)
{
    int pipefd[2];
    if (pipe(pipefd) == -1)
    {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        setenv("REDIRECT_STATUS", "200", 1);
        setenv("REQUEST_METHOD", "GET", 1);
        setenv("SCRIPT_FILENAME", scriptPath, 1);
        if (queryString)
        {
            setenv("QUERY_STRING", queryString, 1);
        }

        execl(PHP_CGI_PATH, PHP_CGI_PATH, NULL);
        perror("execl php-cgi");
        exit(EXIT_FAILURE);
    }
    else
    {
        close(pipefd[1]);

        char header[BUFFER_SIZE];
        char timeBuf[TIMEBUF_SIZE];
        getTimeString(timeBuf);

        snprintf(header, BUFFER_SIZE,
                 "HTTP/1.1 200 OK\r\n"
                 "Date: %s\r\n"
                 "Server: C-Server/1.0\r\n"
                 "Connection: close\r\n",
                 timeBuf);
        sendAll(clientSocket, header, strlen(header));

        char buffer[BUFFER_SIZE];
        ssize_t bytesRead;
        while ((bytesRead = read(pipefd[0], buffer, BUFFER_SIZE)) > 0)
        {
            if (sendAll(clientSocket, buffer, bytesRead) == -1)
            {
                break;
            }
        }

        if (bytesRead == -1)
        {
            perror("read from pipe failed");
        }

        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return 0;
    }
}

void getFileURL(const char *route, char *fileURL, size_t fileURLSize)
{
    char tempRoute[ROUTE_SIZE];
    strncpy(tempRoute, route, ROUTE_SIZE - 1);
    tempRoute[ROUTE_SIZE - 1] = '\0';

    char *question = strrchr(tempRoute, '?');
    if (question)
    {
        *question = '\0';
    }

    snprintf(fileURL, fileURLSize, "%s%s", ROOT_DIR, tempRoute);
}

void getMimeType(const char *file, char *mime)
{
    const char *dot = strrchr(file, '.');
    if (dot == NULL)
        strcpy(mime, "application/octet-stream");
    else if (strcmp(dot, ".php") == 0)
        strcpy(mime, "text/html");
    else if (strcmp(dot, ".html") == 0)
        strcpy(mime, "text/html");
    else if (strcmp(dot, ".css") == 0)
        strcpy(mime, "text/css");
    else if (strcmp(dot, ".js") == 0)
        strcpy(mime, "application/javascript");
    else if (strcmp(dot, ".json") == 0)
        strcpy(mime, "application/json");
    else if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        strcpy(mime, "image/jpeg");
    else if (strcmp(dot, ".png") == 0)
        strcpy(mime, "image/png");
    else if (strcmp(dot, ".gif") == 0)
        strcpy(mime, "image/gif");
    else if (strcmp(dot, ".svg") == 0)
        strcpy(mime, "image/svg+xml");
    else if (strcasecmp(dot, ".txt") == 0)
        strcpy(mime, "text/plain");
    else if (strcasecmp(dot, ".pdf") == 0)
        strcpy(mime, "application/pdf");
    else if (strcmp(dot, ".ico") == 0)
        strcpy(mime, "image/x-icon");
    else if (strcmp(dot, ".mp4") == 0)
        strcpy(mime, "video/mp4");
    else if (strcmp(dot, ".mp3") == 0)
        strcpy(mime, "audio/mpeg");
    else if (strcmp(dot, ".webp") == 0)
        strcpy(mime, "image/webp");
    else
        strcpy(mime, "application/octet-stream");
}

void getTimeString(char *timeBuf)
{
    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = gmtime(&rawtime);
    strftime(timeBuf, TIMEBUF_SIZE, "%a, %d %b %Y %H:%M:%S GMT", timeinfo);
}

int resolvePathSafe(const char *fileURL, char *resolvedPath, size_t resolvedSize)
{
    char realRoot[PATH_MAX];
    if (!realpath(ROOT_DIR, realRoot))
    {
        perror("realpath on ROOT_DIR failed");
        return 0;
    }

    if (!realpath(fileURL, resolvedPath))
    {
        return 0;
    }

    return strncmp(resolvedPath, realRoot, strlen(realRoot)) == 0;
}

ssize_t sendAll(int socket, const void *buffer, size_t length)
{
    size_t totalSent = 0;
    while (totalSent < length)
    {
        ssize_t sent = send(socket, (char *)buffer + totalSent, length - totalSent, 0);
        if (sent <= 0)
        {
            if (sent < 0)
                perror("send failed");
            return sent;
        }
        totalSent += sent;
    }
    return totalSent;
}
