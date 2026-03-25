# Simple Chat — Guía de Comandos
## CC3064 Sistemas Operativos — Proyecto #1

---

## Requisitos previos

Tener **WSL (Ubuntu)** instalado en Windows, o una máquina con Linux.

---

## Paso 1 — Instalar dependencias
> Solo necesitas hacer esto una vez.

```bash
sudo apt update
sudo apt install g++ protobuf-compiler libprotobuf-dev libncurses-dev
```

---

## Paso 2 — Navegar al proyecto

```bash
cd /mnt/c/Users/TU_USUARIO/Downloads/chat_project/chat_project
```
> Reemplaza `TU_USUARIO` con tu nombre de usuario de Windows.

---

## Paso 3 — Compilar

```bash
make
```

Al terminar deberías ver:
```
✓ server built
✓ client built
```

---

## Paso 4 — Correr el servidor
> Solo **una persona** del grupo corre el servidor.

```bash
./server 8080
```

Para saber la IP de tu máquina (la que les das a los demás):
```bash
hostname -I
```

---

## Paso 5 — Correr el cliente
> Cada persona corre esto en su propia terminal con su propio nombre.

```bash
./client <tu_nombre> <IP_del_servidor> 8080
```

**Ejemplo — prueba local (misma máquina):**
```bash
./client alice 127.0.0.1 8080
./client bob   127.0.0.1 8080
```

**Ejemplo — red del salón (día de entrega):**
```bash
./client alice 192.168.1.10 8080
./client bob   192.168.1.10 8080
```

---

## Paso 6 — Comandos dentro del chat

| Comando | Qué hace |
|---|---|
| `hola a todos` | Envía mensaje al chat general (broadcast) |
| `/dm <usuario> <mensaje>` | Envía mensaje privado a un usuario |
| `/list` | Lista todos los usuarios conectados |
| `/info <usuario>` | Muestra IP y status de un usuario |
| `/status active` | Cambia tu status a ACTIVO |
| `/status busy` | Cambia tu status a OCUPADO |
| `/status inactive` | Cambia tu status a INACTIVO |
| `/help` | Muestra todos los comandos disponibles |
| `/quit` | Salir del chat |

---

## Recompilar desde cero

Si necesitas borrar todo y volver a compilar:
```bash
make clean
make
```

---

## Notas para el día de entrega

- Solo **una máquina** corre el servidor.
- Todos los clientes se conectan a la **IP de esa máquina**.
- Llevar **cable de red RJ45** (y adaptador si tu laptop lo necesita).
- Subir el código a **Canvas** antes de la presentación.