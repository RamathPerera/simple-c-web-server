# Simple C Web Server (Linux Only)

This is a lightweight web server written in C for **Linux systems only**.  
It serves static files (HTML, CSS, JS, images, PDFs, etc.) and can execute PHP scripts using **php-cgi**.

---

## Prerequirements

Must have the following installed:

- **GCC** – to compile the server
- **php-cgi** – required only if need PHP file support

---

## Install Prerequirements (If Not Already Installed)

Install GCC
```sh
sudo apt update
sudo apt install build-essential
```

Install PHP CGI
```sh
sudo apt install php-cgi
```

---

## How to Compile

Run this inside the project folder:
```sh
gcc server.c -o server
```

---

## How to Run

Make sure the project contains an htdocs/ folder
(The project already includes sample HTML files, images, PDFs, etc.)

Start the server:
```sh
./server
```

Then open the browser and visit:
```sh
http://localhost:2728/
```
