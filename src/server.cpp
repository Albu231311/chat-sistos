#include <iostream>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

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

// Configuration 
constexpr int INACTIVITY_SECS  = 30;
constexpr int INACTIVITY_CHECK = 10;

//Shared state 
struct UserSession {
    std::string            username;
    std::string            ip;
    int                    fd;
    chat::StatusEnum       status;
    std::chrono::steady_clock::time_point last_active;
};

std::map<std::string, UserSession> gUsers;
std::mutex                          gUsersMtx;
std::atomic<bool>                   gRunning{true};

static std::string statusStr(chat::StatusEnum s) {
    switch (s) {
        case chat::ACTIVE:         return "ACTIVE";
        case chat::DO_NOT_DISTURB: return "BUSY";
        case chat::INVISIBLE:      return "INACTIVE";
        default:                   return "UNKNOWN";
    }
}

// send ServerResponse 
static void sendResp(int fd, int code, const std::string& msg, bool ok) {
    chat::ServerResponse r;
    r.set_status_code(code);
    r.set_message(msg);
    r.set_is_successful(ok);
    std::string p;
    (void)r.SerializeToString(&p);
    sendMsg(fd, TYPE_SERVER_RESP, p);
}

// broadcast mensaje de sistema a todos excepto quien lo causó 

static void broadcastSystemMsg_locked(const std::string& text, const std::string& excludeUser = "") {
    chat::BroadcastDelivery bc;
    bc.set_message(text);
    bc.set_username_origin("[Sistema]");
    std::string p;
    (void)bc.SerializeToString(&p);
    for (auto& [n, s] : gUsers) {
        if (n != excludeUser)
            sendMsg(s.fd, TYPE_BROADCAST, p);
    }
}

//  broadcast mensaje general a todos
static void broadcastAll(const std::string& text, const std::string& origin) {
    chat::BroadcastDelivery bc;
    bc.set_message(text);
    bc.set_username_origin(origin);
    std::string p;
    (void)bc.SerializeToString(&p);

    std::lock_guard<std::mutex> lk(gUsersMtx);
    for (auto& [name, sess] : gUsers)
        sendMsg(sess.fd, TYPE_BROADCAST, p);
}

//  Per-client thread
static void handleClient(int clientFd, std::string clientIp) {
    std::string username;

    auto touchActivity = [&]() {
        std::lock_guard<std::mutex> lk(gUsersMtx);
        auto it = gUsers.find(username);
        if (it != gUsers.end()) {
            it->second.last_active = std::chrono::steady_clock::now();
            if (it->second.status == chat::INVISIBLE) {
                it->second.status = chat::ACTIVE;
                // Notificar al propio cliente que volvió a ACTIVE
                sendResp(clientFd, 206, "ACTIVE", true);
                
                broadcastSystemMsg_locked(username + " volvio a estar ACTIVE");
            }
        }
    };

    while (true) {
        uint8_t    type;
        std::string payload;

        if (!recvMsg(clientFd, type, payload))
            break;

        if (!username.empty())
            touchActivity();

        switch (type) {

        // ── 1. Register 
        case TYPE_REGISTER: {
            chat::Register msg;
            if (!msg.ParseFromString(payload)) break;

            std::string uname = msg.username();

            
            if (uname.empty()) {
                sendResp(clientFd, 400, "El username no puede estar vacio", false);
                break;
            }
            if (uname.find(' ') != std::string::npos) {
                sendResp(clientFd, 400, "El username no puede tener espacios", false);
                break;
            }
            if (uname.size() > 20) {
                sendResp(clientFd, 400, "El username no puede tener mas de 20 caracteres", false);
                break;
            }

            std::lock_guard<std::mutex> lk(gUsersMtx);

            if (gUsers.count(uname)) {
                sendResp(clientFd, 409, "El username '" + uname + "' ya esta en uso", false);
                break;
            }

            username = uname;
            gUsers[username] = {
                username, clientIp, clientFd,
                chat::ACTIVE,
                std::chrono::steady_clock::now()
            };
            std::cout << "[+] Registrado: " << username << " @ " << clientIp << "\n";
            sendResp(clientFd, 200, "Bienvenido, " + username + "!", true);
            
            broadcastSystemMsg_locked(username + " se unio al chat", username);
            break;
        }

        // ── 2. General broadcast 
        case TYPE_MSG_GENERAL: {
            if (username.empty()) { sendResp(clientFd, 401, "No registrado", false); break; }
            chat::MessageGeneral msg;
            if (!msg.ParseFromString(payload)) break;
            
            std::string text = msg.message();
            bool allSpaces = text.find_first_not_of(' ') == std::string::npos;
            if (text.empty() || allSpaces) {
                sendResp(clientFd, 400, "El mensaje no puede estar vacio", false);
                break;
            }
            broadcastAll(text, msg.username_origin());
            break;
        }

        // ── 3. Direct message
        case TYPE_MSG_DM: {
            if (username.empty()) { sendResp(clientFd, 401, "No registrado", false); break; }
            chat::MessageDM msg;
            if (!msg.ParseFromString(payload)) break;

            if (msg.username_des() == username) {
                sendResp(clientFd, 400, "No puedes enviarte un DM a ti mismo", false);
                break;
            }

            
            {
                std::string dmText = msg.message();
                if (dmText.empty() || dmText.find_first_not_of(' ') == std::string::npos) {
                    sendResp(clientFd, 400, "El mensaje no puede estar vacio", false);
                    break;
                }
            }

            std::lock_guard<std::mutex> lk(gUsersMtx);
            auto it = gUsers.find(msg.username_des());
            if (it == gUsers.end()) {
                sendResp(clientFd, 404, "Usuario '" + msg.username_des() + "' no encontrado", false);
                break;
            }

            chat::ForDm dm;
            dm.set_username_des(username);   // sender
            dm.set_message(msg.message());
            std::string p;
            (void)dm.SerializeToString(&p);
            sendMsg(it->second.fd, TYPE_FOR_DM, p);
            sendResp(clientFd, 200, "DM enviado a " + msg.username_des(), true);
            break;
        }

        // ── 4. Change status
        case TYPE_CHANGE_STATUS: {
            if (username.empty()) { sendResp(clientFd, 401, "No registrado", false); break; }
            chat::ChangeStatus msg;
            if (!msg.ParseFromString(payload)) break;

            {
                std::lock_guard<std::mutex> lk(gUsersMtx);
                gUsers[username].status = msg.status();
                
                broadcastSystemMsg_locked(username + " cambio su estado a " + statusStr(msg.status()));
            }
            std::cout << "[~] " << username << " -> " << statusStr(msg.status()) << "\n";
            sendResp(clientFd, 200, "Estado actualizado", true);
            break;
        }

        // ── 5. List users
        case TYPE_LIST_USERS: {
            chat::AllUsers au;
            {
                std::lock_guard<std::mutex> lk(gUsersMtx);
                for (auto& [n, s] : gUsers) {
                    if (s.status != chat::INVISIBLE) {
                        au.add_usernames(n);
                        au.add_status(s.status);
                    }
                }
            }
            std::string p;
            (void)au.SerializeToString(&p);
            sendMsg(clientFd, TYPE_ALL_USERS, p);
            break;
        }

        // ── 6. Get user info
        case TYPE_GET_USER_INFO: {
            chat::GetUserInfo msg;
            if (!msg.ParseFromString(payload)) break;

            std::lock_guard<std::mutex> lk(gUsersMtx);
            auto it = gUsers.find(msg.username_des());
            if (it == gUsers.end()) {
                sendResp(clientFd, 404, "Usuario '" + msg.username_des() + "' no encontrado", false);
                break;
            }
            chat::GetUserInfoResponse resp;
            resp.set_ip_address(it->second.ip);
            resp.set_username(it->second.username);
            resp.set_status(it->second.status);
            std::string p;
            (void)resp.SerializeToString(&p);
            sendMsg(clientFd, TYPE_USER_INFO_RESP, p);
            break;
        }

        // ── 7. Quit
        case TYPE_QUIT:
            goto cleanup;

        default:
            std::cerr << "[!] Tipo de mensaje desconocido: " << (int)type << "\n";
            break;
        }
    }

cleanup:
    if (!username.empty()) {
        std::lock_guard<std::mutex> lk(gUsersMtx);
        gUsers.erase(username);
        std::cout << "[-] Desconectado: " << username << "\n";
        
        broadcastSystemMsg_locked(username + " salio del chat");
    } else {
        std::cout << "[-] Cliente sin registrar desconectado: " << clientIp << "\n";
    }
    close(clientFd);
}

// ── Background inactivity checker 
static void inactivityChecker() {
    while (gRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(INACTIVITY_CHECK));
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lk(gUsersMtx);
        
        for (auto& [name, sess] : gUsers) {
            if (sess.status == chat::INVISIBLE) continue;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - sess.last_active).count();
            if (elapsed >= INACTIVITY_SECS) {
                sess.status = chat::INVISIBLE;
                std::cout << "[~] " << name << " paso a INACTIVE\n";
                sendResp(sess.fd, 205, "INACTIVE", true);
                
                broadcastSystemMsg_locked(name + " paso a INACTIVE por inactividad");
            }
        }
    }
}

// ── main 
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Uso: " << argv[0] << " <puerto>\n";
        return 1;
    }

    
    int port;
    try {
        port = std::stoi(argv[1]);
        if (port <= 0 || port > 65535) throw std::out_of_range("puerto fuera de rango");
    } catch (const std::exception& e) {
        std::cerr << "Puerto invalido: " << argv[1] << "\n";
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    int srvFd = socket(AF_INET, SOCK_STREAM, 0);
    if (srvFd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srvFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srvFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(srvFd, 20) < 0) { perror("listen"); return 1; }

    std::cout << "+======================================+\n";
    std::cout << "|   Chat Server - puerto: " << port << "          |\n";
    std::cout << "|   Inactividad: " << INACTIVITY_SECS << "s                   |\n";
    std::cout << "+======================================+\n";

    std::thread(inactivityChecker).detach();

    while (gRunning) {
        sockaddr_in cliAddr{};
        socklen_t   cliLen = sizeof(cliAddr);
        int cliFd = accept(srvFd, reinterpret_cast<sockaddr*>(&cliAddr), &cliLen);
        if (cliFd < 0) continue;

       
        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliAddr.sin_addr, ipBuf, sizeof(ipBuf));
        std::string ip(ipBuf);

        std::cout << "[>] Nueva conexion desde " << ip << "\n";
        std::thread(handleClient, cliFd, ip).detach();
    }

    close(srvFd);
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}