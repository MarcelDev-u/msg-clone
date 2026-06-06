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

Nginx robi publiczny routing:

```text
https://msg-clone.marceldev-u.my -> http://127.0.0.1:8600
```

## Ochrona przed przeciazeniem

Konfiguracja produkcyjnego Nginx jest wersjonowana w `deploy/nginx/`.

- HTTP: 5 requestow/s na IP, burst 15
- WebSocket handshake: 6/min na IP, burst 3
- WebSocket: maksymalnie 6 polaczen na IP i 120 lacznie
- backend: maksymalnie 128 klientow
- wiadomosci: 2/s na klienta, burst 5
- wiadomosci globalnie: 40/s, burst 80
- SQLite zachowuje maksymalnie okolo 10000 ostatnich wiadomosci
- kontener ma limity CPU, RAM, PID i deskryptorow plikow

Nadmiarowy ruch HTTP i nowe polaczenia dostaja `429`. Wolny lub niekompletny
klient WebSocket nie blokuje pozostalych klientow.
