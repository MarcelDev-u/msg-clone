# msg-clone

Prosty serwer czatu napisany w C.

Strona produkcyjna:

https://msg-clone.marceldev-u.my

## Co tu jest

Ten projekt ma maly serwer HTTP + WebSocket:

- `/` serwuje `public/index.html`
- `/chat` serwuje `public/chat.html`
- `/ws` obsluguje WebSocket czatu
- wiadomosci sa zapisywane w SQLite
- historia czatu jest trzymana w `chat.db`

Kod jest celowo prosty:

- `src/server.c` - HTTP, routing, petla serwera
- `src/websocket.c` - WebSocket handshake i ramki
- `src/chat_db.c` - SQLite
- `include/app.h` - wspolne stale i deklaracje

## Lokalnie

Zbuduj:

```sh
make
```

Uruchom:

```sh
./build/server
```

Otworz:

```text
http://127.0.0.1:8080
```

Stop:

```text
Ctrl+C
```

Czyszczenie builda:

```sh
make clean
```

## Docker

Docker uruchamia tylko aplikacje. Nginx jest poza kontenerem.

```sh
docker compose up -d --build
```

Kontener wystawia aplikacje tylko lokalnie na maszynie:

```text
127.0.0.1:8600 -> container:8080
```

Dane czatu sa w wolumenie Dockera:

```text
chat-data:/data/chat.db
```

## Deploy na VPS

Na VPS repo lezy tutaj:

```text
/home/ubuntu/msg-clone
```

Standardowy deploy:

```sh
cd /home/ubuntu/msg-clone
git pull
docker compose up -d --build
```

Nginx robi publiczny routing:

```text
https://msg-clone.marceldev-u.my -> http://127.0.0.1:8600
```

Certyfikat HTTPS jest z Let's Encrypt / certbot.

DNS:

```text
A  msg-clone.marceldev-u.my  51.38.127.224
```

## Przydatne komendy na VPS

Status kontenera:

```sh
cd /home/ubuntu/msg-clone
docker compose ps
```

Logi aplikacji:

```sh
docker compose logs -f app
```

Test lokalnego upstreamu:

```sh
curl -i http://127.0.0.1:8600/
```

Test nginx:

```sh
curl -i https://msg-clone.marceldev-u.my/
```

Sprawdzenie nginx:

```sh
sudo nginx -t
sudo systemctl reload nginx
```

## Uwaga

Serwer jest edukacyjny/prosty. Nie jest frameworkiem webowym i nie udaje produkcyjnego backendu z auth, rate limitingiem, migracjami itd. Publiczny ruch powinien isc przez nginx, a sam kontener powinien zostac na `127.0.0.1`.
