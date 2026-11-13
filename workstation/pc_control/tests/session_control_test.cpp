#include "../workstation_session.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <chrono>
#include <iostream>
#include <thread>

namespace {
void status() {
    std::cout << "status=connected project= program= mode=2 enabled=1 state=2 line=1 "
                 "continuous=1 error=0 motion_rest=1 command_busy=0\n" << std::flush;
}

// Exercise the real Session pipe/queue without opening a robot connection.
int helper() {
    std::cout << "robot=SR175-3200 cabinet=SRC4L6 serial=test target=real ip=127.0.0.1 access=start_stop\n";
    status();
    std::string command;
    while (std::getline(std::cin, command)) {
        std::cout << "received=" << command << '\n' << std::flush;
        if (command == "quit") return 0;
        if (command == "stop" || command == "estop") {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            status();
            std::cout << "command_done=" << command << '\n' << std::flush;
        } else status();
    }
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "real") return helper();
    QCoreApplication app(argc, argv);
    Session session({}, QCoreApplication::applicationFilePath(), {}, {});
    QStringList received;
    session.note = [&](const QString& line) {
        if (line.startsWith("received=")) received << line.mid(9);
    };
    int failures = 0;
    const auto check = [&](bool ok, const char* reason) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << reason << '\n'; }
    };
    const QStringList commands{"sendrun unused.txt", "runproj Example main", "start", "jog J 1+ 1", "returnhome 1", "auto"};
    int phase = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (elapsed.elapsed() > 5000) {
            check(false, "session control test timed out");
            app.exit(1);
            return;
        }
        if (!session.fresh() || session.waitingStop || session.waitingEstop || !session.pending.isEmpty()) return;
        if (phase < commands.size()) {
            check(session.request(commands[phase]), "could not queue command");
            session.stop();
            check(session.pending.isEmpty(), "STOP left an unsent command queued");
            check(!session.request("start"), "accepted START before STOP acknowledgement");
            ++phase;
        } else if (phase == commands.size()) {
            check(session.emergencyStop(), "could not submit emergency stop");
            check(!session.request("start"), "accepted START before ESTOP acknowledgement");
            ++phase;
        } else {
            for (const auto& command : commands)
                check(!received.contains(command), "cancelled command reached control process");
            check(received.count("stop") == commands.size(), "STOP was not delivered for every cancellation");
            check(received.contains("estop"), "ESTOP did not reach control process");
            session.disconnect();
            check(!session.connected(), "helper did not exit cleanly");
            std::cout << "session_control_test: failures=" << failures << '\n';
            app.exit(failures ? 1 : 0);
        }
    });
    timer.start(10);
    session.connectReal("127.0.0.1");
    return app.exec();
}
