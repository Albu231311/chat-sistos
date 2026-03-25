
#include <ncurses.h>
#include <locale.h>
#include <iostream>
#include <string>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstring>
#include <csignal>
#include <ctime>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdexcept>
#include <cstdlib>

#include "utils.h"
#include "common.pb.h"
#include "register.pb.h"
#include "message_general.pb.h"
#include "message_dm.pb.h"
#include "change_status.pb.h"
#include "list_users.pb.h"
#include "get_user_info.pb.h"
#include "quit.pb.h"
#include "all_users.pb.h"
#include "for_dm.pb.h"
#include "broadcast_messages.pb.h"
#include "get_user_info_response.pb.h"
#include "server_response.pb.h"


int                         gSockFd = -1;
std::string                 gUsername;
std::string                 gMyIp;
std::atomic<chat::StatusEnum> gStatus{chat::ACTIVE};
std::atomic<bool>           gRunning{true};
std::atomic<bool>           gListRequested{false};


WINDOW* gHdrWin  = nullptr;   
WINDOW* gMsgWin  = nullptr;   
WINDOW* gInpWin  = nullptr;   
std::mutex gNcMtx;            

int gMsgRows = 0;   // height of gMsgWin
int gCols    = 0;   // terminal width

// Message log: (text, color_pair)
struct MsgEntry { std::string text; int color; };
std::deque<MsgEntry> gMsgLog;

// Color pairs 
// 1 = cyan    (header, system separators)
// 2 = green   (general broadcast)
// 3 = yellow  (DMs)
// 4 = red     (errors, system alerts)
// 5 = white   (normal info text)
// 6 = magenta (own broadcast echoed back)

static std::string statusStr(chat::StatusEnum s) {
    switch (s) {
        case chat::ACTIVE:         return "ACTIVE";
        case chat::DO_NOT_DISTURB: return "BUSY";
        case chat::INVISIBLE:      return "INACTIVE";
        default:                   return "UNKNOWN";
    }
}

static std::string timestamp() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", t);
    return std::string(buf);
}


static void drawHeader() {
    werase(gHdrWin);
    wbkgd(gHdrWin, COLOR_PAIR(1) | A_BOLD);
   
    char buf[256];
    snprintf(buf, sizeof(buf),
             " Simple Chat  |  User: %-16s  |  Status: %-8s  |  /help",
             gUsername.c_str(),
             statusStr(gStatus.load()).c_str());
    mvwaddstr(gHdrWin, 0, 1, buf);
    wrefresh(gHdrWin);
}

// Redraw the message window from gMsgLog
static void redrawMessages() {
    werase(gMsgWin);
    int total = static_cast<int>(gMsgLog.size());
    int start = std::max(0, total - gMsgRows);
    for (int i = start; i < total; ++i) {
        int row = i - start;
        wattron(gMsgWin, COLOR_PAIR(gMsgLog[i].color));
        mvwprintw(gMsgWin, row, 0, "%s", gMsgLog[i].text.c_str());
        wattroff(gMsgWin, COLOR_PAIR(gMsgLog[i].color));
    }
    wrefresh(gMsgWin);
}

// Add a message to the log and refresh the screen
static void addMsg(const std::string& text, int color = 5) {
    std::lock_guard<std::mutex> lk(gNcMtx);
    gMsgLog.push_back({text, color});
    if (static_cast<int>(gMsgLog.size()) > gMsgRows * 5)
        gMsgLog.pop_front();

    redrawMessages();
    // Keep cursor in input area
    wrefresh(gInpWin);
}

// Rebuild header after status change
static void refreshHeader() {
    std::lock_guard<std::mutex> lk(gNcMtx);
    drawHeader();
    wrefresh(gInpWin);
}

// Receiver thread
static void receiverThread() {
    while (gRunning) {
        uint8_t    type;
        std::string payload;

        if (!recvMsg(gSockFd, type, payload)) {
            if (gRunning) {
                addMsg("[!] Connection to server lost. Press any key to exit.", 4);
                gRunning = false;
            }
            break;
        }

        switch (type) {

        case TYPE_BROADCAST: {
            chat::BroadcastDelivery bc;
            (void)bc.ParseFromString(payload);
            
            if (bc.username_origin() == "[Sistema]") {
                addMsg("[Sistema] " + bc.message(), 1);   // cyan
            } else {
                std::string txt = "[" + timestamp() + "] " +
                                  bc.username_origin() + ": " + bc.message();
                int col = (bc.username_origin() == gUsername) ? 6 : 2;
                addMsg(txt, col);
            }
            break;
        }

        case TYPE_FOR_DM: {
            chat::ForDm dm;
            (void)dm.ParseFromString(payload);
            
            std::string txt = "[DM from " + dm.username_des() + " @ " +
                              timestamp() + "]: " + dm.message();
            addMsg(txt, 3);
            break;
        }

        case TYPE_ALL_USERS: {
            
            chat::AllUsers au;
            (void)au.ParseFromString(payload);
            gListRequested = false;   // reset por si acaso
            addMsg("+--- Usuarios Conectados ----------------------------+", 1);
            if (au.usernames_size() == 0) {
                addMsg("|  (ningún usuario visible)                         |", 5);
            } else {
                for (int i = 0; i < au.usernames_size(); i++) {
                    std::string entry = "|  " + au.usernames(i) +
                                        "  [" + statusStr(static_cast<chat::StatusEnum>(au.status(i))) + "]";
                    addMsg(entry, 5);
                }
            }
            addMsg("+----------------------------------------------------+", 1);
            break;
        }

        case TYPE_USER_INFO_RESP: {
            chat::GetUserInfoResponse r;
            (void)r.ParseFromString(payload);
            addMsg("+--- User Info -----------------------------+", 1);
            addMsg("|  Username : " + r.username(), 5);
            addMsg("|  IP       : " + r.ip_address(), 5);
            addMsg("|  Status   : " + statusStr(r.status()), 5);
            addMsg("+-------------------------------------------+", 1);
            break;
        }

        case TYPE_SERVER_RESP: {
            chat::ServerResponse r;
            (void)r.ParseFromString(payload);
            if (r.status_code() == 205) {
                // Servidor puso INACTIVE por inactividad
                gStatus = chat::INVISIBLE;
                refreshHeader();
                addMsg("[System] Fuiste puesto en INACTIVE por inactividad.", 4);
            } else if (r.status_code() == 206) {
                // Servidor restauro a ACTIVE al recibir actividad
                gStatus = chat::ACTIVE;
                refreshHeader();
                addMsg("[System] Tu estado fue restaurado a ACTIVE.", 1);
            } else if (!r.is_successful()) {
                addMsg("[Error] " + r.message(), 4);
            }
            break;
        }

        default:
            break;
        }
    }
}

// Command processor

// Elimina espacios al inicio y al final de un string
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(' ');
    return s.substr(start, end - start + 1);
}

static void processCommand(const std::string& rawLine) {
    
    std::string line = trim(rawLine);
    if (line.empty()) return;

    // /help
    if (line == "/help") {
        addMsg("+--- Commands --------------------------------------------------+", 1);
        addMsg("|  <message>                 broadcast to all                   |", 5);
        addMsg("|  /dm <user> <message>      send a direct message              |", 5);
        addMsg("|  /list                     list connected users               |", 5);
        addMsg("|  /info <user>              get info about a user              |", 5);
        addMsg("|  /status active|busy|inactive  change your status             |", 5);
        addMsg("|  /help                     show this help                     |", 5);
        addMsg("|  /quit                     exit                               |", 5);
        addMsg("+---------------------------------------------------------------+", 1);
        return;
    }

    // /quit
    if (line == "/quit") {
        chat::Quit q;
        q.set_quit(true);
        q.set_ip(gMyIp);
        std::string p;
        (void)q.SerializeToString(&p);
        sendMsg(gSockFd, TYPE_QUIT, p);
        gRunning = false;
        return;
    }

    // /list
    if (line == "/list") {
        gListRequested = true; 
        chat::ListUsers lu;
        lu.set_username(gUsername);
        lu.set_ip(gMyIp);
        std::string p;
        (void)lu.SerializeToString(&p);
        sendMsg(gSockFd, TYPE_LIST_USERS, p);
        return;
    }

    //  /info
    if (line == "/info") {
        addMsg("[Error] Formato: /info <usuario>", 4);
        return;
    }
    if (line.size() > 6 && line.substr(0, 6) == "/info ") {
        std::string target = trim(line.substr(6));  
        if (target.empty()) {
            addMsg("[Error] Formato: /info <usuario>", 4);
            return;
        }
        chat::GetUserInfo gui;
        gui.set_username_des(target);
        gui.set_username(gUsername);
        gui.set_ip(gMyIp);
        std::string p;
        (void)gui.SerializeToString(&p);
        sendMsg(gSockFd, TYPE_GET_USER_INFO, p);
        return;
    }

    //  /status <value> 
    if (line == "/status") {
        addMsg("[Error] Formato: /status active | busy | inactive", 4);
        return;
    }
    if (line.size() > 8 && line.substr(0, 8) == "/status ") {
        std::string val = trim(line.substr(8));   // Bug L fix: trim completo
        chat::StatusEnum newSt;
        if      (val == "active"   || val == "ACTIVE")   newSt = chat::ACTIVE;
        else if (val == "busy"     || val == "BUSY")      newSt = chat::DO_NOT_DISTURB;
        else if (val == "inactive" || val == "INACTIVE")  newSt = chat::INVISIBLE;
        else {
            addMsg("[Error] Formato: /status active | busy | inactive", 4);
            return;
        }
        chat::ChangeStatus cs;
        cs.set_status(newSt);
        cs.set_username(gUsername);
        cs.set_ip(gMyIp);
        std::string p;
        (void)cs.SerializeToString(&p);
        sendMsg(gSockFd, TYPE_CHANGE_STATUS, p);
        gStatus = newSt;
        refreshHeader();
        addMsg("[System] Status cambiado a " + statusStr(newSt), 1);
        return;
    }

    // /dm <user> <message> 
    if (line == "/dm") {
        addMsg("[Error] Formato: /dm <usuario> <mensaje>", 4);
        return;
    }
    if (line.size() > 4 && line.substr(0, 4) == "/dm ") {
        std::string rest = trim(line.substr(4)); 
        auto sp = rest.find(' ');
        if (sp == std::string::npos) {
            addMsg("[Error] Formato: /dm <usuario> <mensaje>", 4);
            return;
        }
        std::string target = trim(rest.substr(0, sp));   // trim al target
        std::string msg    = trim(rest.substr(sp + 1));  // trim al mensaje
        if (target.empty() || msg.empty()) {
            addMsg("[Error] Formato: /dm <usuario> <mensaje>", 4);
            return;
        }

        chat::MessageDM dm;
        dm.set_message(msg);
        dm.set_status(gStatus.load());
        dm.set_username_des(target);
        dm.set_ip(gMyIp);
        std::string p;
        (void)dm.SerializeToString(&p);
        sendMsg(gSockFd, TYPE_MSG_DM, p);
        addMsg("[DM to " + target + " @ " + timestamp() + "]: " + msg, 3);
        return;
    }

    //  Comando con / no reconocido 
    if (!line.empty() && line[0] == '/') {
        addMsg("[Error] Comando no reconocido. Comandos disponibles:", 4);
        addMsg("  /dm <usuario> <mensaje>       mensaje privado", 4);
        addMsg("  /list                          ver usuarios conectados", 4);
        addMsg("  /info <usuario>                ver info de usuario", 4);
        addMsg("  /status active|busy|inactive   cambiar estado", 4);
        addMsg("  /help  /quit", 4);
        return;
    }

    // General broadcast 
    chat::MessageGeneral mg;
    mg.set_message(line);
    mg.set_status(gStatus.load());
    mg.set_username_origin(gUsername);
    mg.set_ip(gMyIp);
    std::string p;
    (void)mg.SerializeToString(&p);
    sendMsg(gSockFd, TYPE_MSG_GENERAL, p);
   
}

// ncurses initialisation 
static void initUI() {
    setlocale(LC_ALL, "");
    initscr();
    start_color();
    use_default_colors();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    init_pair(1, COLOR_CYAN,    -1);
    init_pair(2, COLOR_GREEN,   -1);
    init_pair(3, COLOR_YELLOW,  -1);
    init_pair(4, COLOR_RED,     -1);
    init_pair(5, COLOR_WHITE,   -1);
    init_pair(6, COLOR_MAGENTA, -1);

    int rows;
    getmaxyx(stdscr, rows, gCols);
    
    if (rows < 5 || gCols < 20) {
        endwin();
        std::cerr << "Terminal demasiado pequeno. Minimo 5 filas x 20 columnas.\n";
        exit(1);
    }
    gMsgRows = rows - 3;

    gHdrWin = newwin(1,        gCols, 0,           0);
    gMsgWin = newwin(gMsgRows, gCols, 1,           0);
    gInpWin = newwin(2,        gCols, rows - 2,    0);

    scrollok(gMsgWin, TRUE);
    idlok(gMsgWin, TRUE);

    drawHeader();
    wrefresh(gHdrWin);

    
    mvwhline(gInpWin, 0, 0, ACS_HLINE, gCols);
    mvwprintw(gInpWin, 1, 0, "> ");
    wrefresh(gInpWin);
    wrefresh(gMsgWin);

    
    wtimeout(gInpWin, 100);
}

// main 
int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <username> <server_ip> <port>\n";
        return 1;
    }

    gUsername           = argv[1];
    std::string srvIp   = argv[2];

    
    if (gUsername.empty() || gUsername.find(' ') != std::string::npos || gUsername.size() > 20) {
        std::cerr << "Username invalido. No puede estar vacio, tener espacios, ni exceder 20 caracteres.\n";
        return 1;
    }

    
    int port;
    try {
        port = std::stoi(argv[3]);
        if (port <= 0 || port > 65535) throw std::out_of_range("rango");
    } catch (...) {
        std::cerr << "Puerto invalido: " << argv[3] << "\n";
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // Connect to server
    gSockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (gSockFd < 0) { perror("socket"); return 1; }

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(port);
    if (inet_pton(AF_INET, srvIp.c_str(), &srv.sin_addr) <= 0) {
        std::cerr << "Invalid server IP: " << srvIp << "\n";
        return 1;
    }
    if (connect(gSockFd, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) < 0) {
        perror("connect"); return 1;
    }

    // Determine our local IP from the connected socket
    sockaddr_in local{};
    socklen_t llen = sizeof(local);
    getsockname(gSockFd, reinterpret_cast<sockaddr*>(&local), &llen);
    char ipBuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, ipBuf, sizeof(ipBuf));
    gMyIp = std::string(ipBuf);

    // Register
    {
        chat::Register reg;
        reg.set_username(gUsername);
        reg.set_ip(gMyIp);
        std::string p;
        (void)reg.SerializeToString(&p);
        sendMsg(gSockFd, TYPE_REGISTER, p);
    }

    // Wait for server's registration response
    {
        uint8_t    type;
        std::string rp;
        if (!recvMsg(gSockFd, type, rp)) {
            std::cerr << "No response from server during registration.\n";
            close(gSockFd);
            return 1;
        }
        if (type == TYPE_SERVER_RESP) {
            chat::ServerResponse resp;
            (void)resp.ParseFromString(rp);
            if (!resp.is_successful()) {
                std::cerr << "Registration failed: " << resp.message() << "\n";
                close(gSockFd);
                return 1;
            }
        }
    }

    // Init ncurses UI
    initUI();

    addMsg("+================================================+", 1);
    addMsg("|  Welcome to Simple Chat!  Type /help           |", 1);
    addMsg("+================================================+", 1);

    // Start receiver thread 
    std::thread recvThr(receiverThread);

    //  Input loop
    std::string inputBuf;

    while (gRunning) {
        
        {
            std::lock_guard<std::mutex> lk(gNcMtx);
            wmove(gInpWin, 1, 2 + static_cast<int>(inputBuf.size()));
            wrefresh(gInpWin);
        }

        int ch = wgetch(gInpWin);

        if (ch == ERR) continue;   // timeout – loop back and check gRunning
        if (!gRunning) break;

        if (ch == '\n' || ch == KEY_ENTER) {
            std::string cmd = inputBuf;
            inputBuf.clear();
            {
                std::lock_guard<std::mutex> lk(gNcMtx);
                wmove(gInpWin, 1, 0);
                wclrtoeol(gInpWin);
                wprintw(gInpWin, "> ");
                wrefresh(gInpWin);
            }
            processCommand(cmd);

        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (!inputBuf.empty()) {
                inputBuf.pop_back();
                std::lock_guard<std::mutex> lk(gNcMtx);
                // Erase the last character on screen
                int pos = 2 + static_cast<int>(inputBuf.size());
                mvwdelch(gInpWin, 1, pos);
                wrefresh(gInpWin);
            }

        } else if (ch >= 32 && ch < 127) {
            
            if (2 + static_cast<int>(inputBuf.size()) < gCols - 1) {
                inputBuf += static_cast<char>(ch);
                std::lock_guard<std::mutex> lk(gNcMtx);
                waddch(gInpWin, static_cast<chtype>(ch));
                wrefresh(gInpWin);
            }
        }
    }

    
    gRunning = false;
    
    shutdown(gSockFd, SHUT_RDWR);
    close(gSockFd);
    recvThr.join();
    endwin();
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
