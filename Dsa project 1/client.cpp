// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <fcntl.h>
#include <cerrno>
#include <sstream>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader 
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = recv(sock, buf + got, n - got, 0);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        got += (size_t)r;
    }
    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        sent += (size_t)r;
    }
    return true;
}

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        connect(s, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(s);
        return -1;
    }
    timeval tv;
    tv.tv_sec = CLIENT_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return s;
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    FILE *in = fopen(sourcePath, "rb");
    if (!in)
        return false;
    char status = STATUS_OK;
    CRC32 crc;
    crc.update(&status, 1);
    bool ok = sendAll(sock, &status, 1);
    char buf[IO_BUFFER_SIZE];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        crc.update(buf, n);
        ok = sendAll(sock, buf, n);
    }
    fclose(in);
    if (ok)
    {
        char trailer[CHECKSUM_SIZE];
        encode32(crc.value(), trailer);
        ok = sendAll(sock, trailer, CHECKSUM_SIZE);
    }
    shutdown(sock, SHUT_WR);
    return ok;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    char status;
    errno = 0;
    if (!recvAll(sock, &status, 1))
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            outError = "timed out waiting for the reply";
        else
            outError = "no reply (waited too long in the queue, or the connection was closed)";
        return false;
    }
    if (status != STATUS_OK && status != STATUS_ERROR)
    {
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    TrailerChecker checker;
    checker.addPrefixByte(status);
    FILE *out = nullptr;
    if (status == STATUS_OK)
    {
        out = fopen(outTdbgPath, "wb");
        if (!out)
        {
            outError = "cannot save the .tdbg file";
            return false;
        }
    }
    string errorText;
    char buf[IO_BUFFER_SIZE];
    bool lost = false;
    while (true)
    {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            lost = true;
            break;
        }
        if (n == 0)
            break;
        string safe = checker.feed(buf, (size_t)n);
        if (out)
            fwrite(safe.data(), 1, safe.size(), out);
        else
            errorText += safe;
    }
    if (out)
        fclose(out);
    if (lost)
    {
        if (out)
            remove(outTdbgPath);
        outError = "connection lost while receiving the reply";
        return false;
    }
    if (!checker.verify())
    {
        if (out)
            remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (status == STATUS_ERROR)
    {
        outError = errorText;
        return false;
    }
    return true;
}
bool requestDebugFile(const char *host, int port, const char *sourcePath, const char *outTdbgPath, string &outError)
{
    FILE *probe = fopen(sourcePath, "rb");
    if (!probe)
    {
        outError = string("cannot open source file ") + sourcePath;
        return false;
    }
    fclose(probe);
    int sock = connectToHost(host, port);
    if (sock < 0)
    {
        outError = "cannot connect to the load balancer";
        return false;
    }
    bool uploaded = uploadSource(sock, sourcePath);
    string tmpPath = string(outTdbgPath) + ".part";
    bool ok = receiveResponse(sock, tmpPath.c_str(), outError);
    close(sock);
    if (!ok)
    {
        if (!uploaded && outError.rfind("no reply", 0) == 0)
            outError = "upload failed, connection lost";
        return false;
    }
    if (rename(tmpPath.c_str(), outTdbgPath) != 0)
    {
        remove(tmpPath.c_str());
        outError = "cannot save the .tdbg file";
        return false;
    }
    return true;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

const int64_t TDBG_HEADER_SIZE = 20;
const int32_t MAX_NAME_LEN = 4096;

bool readExact(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        got += (size_t)r;
    }
    return true;
}
bool readInt32(int fd, int32_t &v)
{
    return readExact(fd, &v, sizeof(int32_t));
}
bool readName(int fd, string &out)
{
    int32_t len;
    if (!readInt32(fd, len) || len < 0 || len > MAX_NAME_LEN)
        return false;
    out.resize(len);
    if (len > 0 && !readExact(fd, &out[0], (size_t)len))
        return false;
    return true;
}
bool readVariable(int fd, Variable &v)
{
    return readName(fd, v.name) && readInt32(fd, v.value);
}
bool readFrame(int fd, Frame &fr)
{
    if (!readName(fd, fr.func_name))
        return false;
    if (!readInt32(fd, fr.argc) || fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME)
        return false;
    for (int32_t i = 0; i < fr.argc; i++)
        if (!readVariable(fd, fr.argv[i]))
            return false;
    if (!readInt32(fd, fr.returnLine))
        return false;
    if (!readInt32(fd, fr.localCount) || fr.localCount < 0 || fr.localCount > MAX_VARS_PER_FRAME)
        return false;
    for (int32_t i = 0; i < fr.localCount; i++)
        if (!readVariable(fd, fr.locals[i]))
            return false;
    return true;
}

bool openTdbg(const char *path, TdbgReader &r)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    TTDBHeader h;
    if (size < TDBG_HEADER_SIZE ||
        !readExact(fd, h.magic, 4) ||
        !readExact(fd, &h.version, sizeof(int32_t)) ||
        !readExact(fd, &h.stepCount, sizeof(int32_t)) ||
        !readExact(fd, &h.indexOffset, sizeof(int64_t)))
    {
        close(fd);
        return false;
    }
    if (memcmp(h.magic, "TTDB", 4) != 0 || h.stepCount <= 0 ||
        h.indexOffset < TDBG_HEADER_SIZE ||
        h.indexOffset + (int64_t)h.stepCount * (int64_t)sizeof(int64_t) > (int64_t)size)
    {
        close(fd);
        return false;
    }
    vector<int64_t> idx(h.stepCount);
    if (lseek(fd, (off_t)h.indexOffset, SEEK_SET) < 0 ||
        !readExact(fd, idx.data(), idx.size() * sizeof(int64_t)))
    {
        close(fd);
        return false;
    }
    for (size_t i = 0; i < idx.size(); i++)
    {
        if (idx[i] < TDBG_HEADER_SIZE || idx[i] >= h.indexOffset)
        {
            close(fd);
            return false;
        }
    }
    r.file_index = fd;
    r.header = h;
    r.index = idx;
    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    r.index.clear();
    r.index.shrink_to_fit();
    if (r.file_index >= 0)
        close((int)r.file_index);
    r.file_index = -1;
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (r.file_index < 0 || step < 0 || step >= r.header.stepCount || step >= (int32_t)r.index.size())
        return nullptr;
    int fd = (int)r.file_index;
    if (lseek(fd, (off_t)r.index[step], SEEK_SET) < 0)
        return nullptr;
    Snapshot *snap = new Snapshot;
    if (!readInt32(fd, snap->stackDepth) || snap->stackDepth < 0 || snap->stackDepth > MAX_STACK_DEPTH)
    {
        delete snap;
        return nullptr;
    }
    for (int32_t d = 0; d < snap->stackDepth; d++)
    {
        if (!readFrame(fd, snap->callStack[d]))
        {
            delete snap;
            return nullptr;
        }
    }
    return snap;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.reader.file_index = -1;
    memset(&s.reader.header, 0, sizeof(s.reader.header));
    s.reader.index.clear();
    s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag)
        closeTdbg(s.reader);
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot *snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (snap == nullptr)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "no active session, use: start <source file>" << endl;
        return false;
    }
    int32_t total = s.reader.header.stepCount;
    int64_t target = s.currentStep + (int64_t)delta;
    if (target < 0 || target >= total)
    {
        cout << "cannot move there: you are at step " << (s.currentStep + 1)
             << " of " << total << " (valid steps are 1 to " << total << ")" << endl;
        return false;
    }
    int64_t old = s.currentStep;
    s.currentStep = target;
    if (!showCurrent(s))
    {
        s.currentStep = old;
        return false;
    }
    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    string err;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), TDBG_FILE, err))
    {
        cout << err << endl;
        return false;
    }
    TdbgReader fresh;
    fresh.file_index = -1;
    if (!openTdbg(TDBG_FILE, fresh))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }
    if (s.active_flag)
        closeTdbg(s.reader);
    s.reader = fresh;
    s.currentStep = 0;
    s.active_flag = 1;
    return showCurrent(s);
}
bool cmdNext(DebugSession &s)
{
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession &s)
{
    return moveBy(s, -1);
}
bool cmdJmpNext(DebugSession &s, int32_t n)
{
    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession &s, int32_t n)
{
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << endl << "=== Step " << step << " of " << totalSteps << " ===" << endl;
    if (s.stackDepth == 0)
    {
        cout << "(empty call stack)" << endl;
        return;
    }
    for (int32_t d = 0; d < s.stackDepth; d++)
    {
        const Frame &fr = s.callStack[d];
        cout << (d == 0 ? "-> " : "   ") << "#" << d << " " << fr.func_name << "(";
        for (int32_t i = 0; i < fr.argc; i++)
        {
            if (i > 0)
                cout << ", ";
            cout << fr.argv[i].name << "=" << fr.argv[i].value;
        }
        cout << ")";
        if (fr.returnLine >= 0)
            cout << "  (return offset " << fr.returnLine << ")";
        cout << endl;
        cout << "      locals: ";
        if (fr.localCount == 0)
            cout << "(none)";
        for (int32_t i = 0; i < fr.localCount; i++)
        {
            if (i > 0)
                cout << ", ";
            cout << fr.locals[i].name << "=" << fr.locals[i].value;
        }
        cout << endl;
    }
}
void printHelp()
{
    // Print the commands
    cout << "Commands:" << endl;
    cout << "  start <source file>   upload the source, download the .tdbg, show step 1" << endl;
    cout << "  next                  go forward one step" << endl;
    cout << "  prv                   go back one step" << endl;
    cout << "  jmp next <n>          go forward n steps" << endl;
    cout << "  jmp prv <n>           go back n steps" << endl;
    cout << "  help                  show this text" << endl;
    cout << "  quit | exit           leave the debugger" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool parseCount(const string &text, int32_t &out)
{
    if (text.empty() || text.size() > 10)
        return false;
    int64_t v = 0;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] < '0' || text[i] > '9')
            return false;
        v = v * 10 + (text[i] - '0');
    }
    if (v < 1 || v > MAX_JUMP)
        return false;
    out = (int32_t)v;
    return true;
}
bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    vector<string> words;
    istringstream iss(line);
    string w;
    while (iss >> w)
        words.push_back(w);
    if (!words.empty() && words[0] == PROGRAM_NAME)
        words.erase(words.begin());
    if (words.empty())
        return true;
    const string &cmd = words[0];
    if (cmd == "start" && words.size() == 2)
        return cmdStart(s, words[1]);
    if (cmd == "next" && words.size() == 1)
        return cmdNext(s);
    if (cmd == "prv" && words.size() == 1)
        return cmdPrv(s);
    if (cmd == "jmp" && words.size() == 3)
    {
        int32_t n = 0;
        if ((words[1] == "next" || words[1] == "prv") && parseCount(words[2], n))
            return words[1] == "next" ? cmdJmpNext(s, n) : cmdJmpPrv(s, n);
    }
    if (cmd == "help" && words.size() == 1)
    {
        printHelp();
        return true;
    }
    if ((cmd == "quit" || cmd == "exit") && words.size() == 1)
    {
        quitRequested = true;
        return true;
    }
    cout << "unknown command" << endl;
    printHelp();
    return false;
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
    if (argc >= 3)
    {
        LB_HOST = argv[1];
        LB_PORT = atoi(argv[2]);
    }
    if (LB_PORT < 1 || LB_PORT > 65535)
    {
        cout << "usage: ./client <lb_ip> <lb_port>" << endl;
        return 1;
    }
    DebugSession session;
    initSession(session);
    cout << "Time-Travel Debugger (" << PROGRAM_NAME << ") connected to "
         << LB_HOST << ":" << LB_PORT << endl;
    printHelp();
    bool quitRequested = false;
    while (true)
    {
        if (quitRequested)
            break;
        cout << PROMPT << flush;
        string line;
        if (!getline(cin, line))
            break;
        runCommand(session, line, quitRequested);
    }
    closeSession(session);
    return 0;
}