// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // first_word + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = nullptr;
        count = 0;
    }
    void push(const T &val)
    {

        // pushes the value on the stack if max limit is not reached yet.
        if (count >= MAX_STACK_DEPTH)
            return;
        Node* new_node = new Node;
        new_node->data = val;
        new_node->next = nullptr;
        if (top == nullptr)
        {
            top = new_node;
            count++;
            return;
        }
        new_node->next = top;
        top = new_node;
        count++;
    }
    T pop()
    {
        // pop the top value on the stack
        Node* temp = top;
        if (temp == nullptr)
            return T();
        T var;
        if (top != nullptr)
        {
            top = top->next;
             var = temp->data;
            delete temp;
            count--;
        }
        return var;
    }
    T &peek()
    {
        // returns the top value on the stack
        return top->data;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        int32_t written = 0;
        Node* temp = top;

        while (temp != nullptr && written < maxLen)
        {
            out[written] = temp->data;
            written++;
            temp = temp->next;
        }

        return written;
    }
};

// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head =tail= nullptr;
        stepCount = 0;
    }
    void record(Snapshot *s)
    {
        // add record in the timeline
        TimelineNode* new_tl = new TimelineNode;
        new_tl->data = s;
        new_tl->next = nullptr;
        if (head == nullptr)
        {
            new_tl->prev = nullptr;
            head = new_tl;
            tail = new_tl;
            stepCount++;
            return;
        }
        new_tl->prev = tail;
        tail->next = new_tl;
        tail = new_tl;
        stepCount++;
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
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
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    // reads the next nonblank line
      while (getline(in, out))
      {
          if (!out.empty())
              return true;
      }

      return false;
}
string firstWord(const string &line)
{
    // returns first word from the input string
    int i = 0;
    string s;
    for (; i < line.size() && line[i] != ' '; i++)
    {
        s += line[i];
    }
  if(i!=0)
    return s;
  return "";
}
string secondWord(const string &line)
{
    // returns the second word
    int i = 0;
    string s;
    for (; i < line.size() && line[i] != ' '; i++);
    while( i < line.size()&& line[i] == ' ')
    i++;
    if (i < line.size())
    {
        for (; i < line.size() && line[i] != ' '; i++)
        {
            s += line[i];
        }
    }
    else
        return "";
    return s;
}
bool validateProgram(const char *sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream input(sourcePath);
    if (!input)
        return false;
    string line="";
    Stack<string> stack;
    while (getline(input, line))
    {
        string word = firstWord(line);
        if (word == "func")
        {
            if (!stack.isEmpty())
                return false;
            stack.push(word);
        }
        if (word == "func_end")
        {
            if (stack.isEmpty())
                return false;
                stack.pop();
            
        }
    }
    return stack.isEmpty();
}
// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    int32_t size = text.size();
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), 1, text.size(), f);
    return offsetField;
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    int64_t offset;
    if(fread(&offset, sizeof(int64_t), 1, f)!=1)
        return -1;
    int32_t size;
    if(fread(&size, sizeof(int32_t), 1, f)!=1)
        return -1;
    if (size < 0)
        return-1;
    outText.resize(size);
    fread(&outText[0], 1, size, f);
    return offset;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    int64_t mainOffset = -1;

    ifstream input(sourcePath);
    if (!input)
    {
        cout << "Error: cannot open " << sourcePath << endl;
        return -1;
    }
    FILE* f = fopen(resolveBinPath, "wb");
    if (!f)
    {
        cout << "Error: cannot create " << resolveBinPath << endl;
        return -1;
    }
    string line;
    int64_t offset = 0; 

    while (getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        string toWrite = line;
        string first_word = firstWord(line);

        if (first_word == "func")
        {
            string name = secondWord(line);
            if (name.empty())
            {
                cout << "Error: func without a name" << endl;
                fclose(f);
                return -1;
            }
            for (int32_t j = 0; j < funcCount; j++)
            {
                if (funcArray[j].funcName == name)
                {
                    cout << "Error: duplicate function " << name << endl;
                    fclose(f);
                    return -1;
                }
            }
            if (funcCount >= MAX_FUNCS)
            {
                cout << "Error: too many functions" << endl;
                fclose(f);
                return -1;
            }
            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = offset;
            funcCount++;
            if (name == "main")
                mainOffset = offset;
        }
        else if (first_word == "call")
        {
            string target = secondWord(line);
            if (target.empty())
            {
                cout << "Error: call without a target" << endl;
                fclose(f);
                return -1;
            }
            if (patchCount >= MAX_PATCHES)
            {
                cout << "Error: too many call instructions" << endl;
                fclose(f);
                return -1;
            }
            int p = (int)line.find(' ');
            while (p < (int)line.size() && line[p] == ' ')
                p++;
            toWrite = line.substr(0, p) + "0000000000" + line.substr(p + target.size());
            patches[patchCount].byteOffsetOfOffsetField = offset + 8 + 4 + (int64_t)p;
            patches[patchCount].targetFuncName = target;
            patchCount++;
        }

        writeResolveRecord(f, offset, toWrite);
        offset += 8 + 4 + (int64_t)toWrite.size(); // uses the FINAL size
    }

    if (mainOffset == -1)
    {
        cout << "Error: main function not found" << endl;
        fclose(f);
        return -1;
    }
    for (int32_t i = 0; i < patchCount; i++)
    {
        int64_t targetOffset = -1;
        bool found = false;
        for (int32_t j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                found = true;
                break;
            }
        }
        if (!found)
        {
            cout << "Error: call to undefined function " << patches[i].targetFuncName << endl;
            fclose(f);
            return -1;
        }

        char buf[16];
        snprintf(buf, sizeof(buf), "%010lld", (long long)targetOffset);
        fseek(f, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(buf, 1, 10, f);
    }

    fclose(f);
    return mainOffset;
}
 

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    int32_t count = 0;
    int32_t i = 0;
    int32_t n = line.size();

    while (i < n && count < maxTokens)
    {
        while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
            i++;
        if (i >= n)
            break;
        string word = "";
        while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
        {
            word += line[i];
            i++;
        }
        if (count == 0)
            tokens[count].type = KEYWORD;
        else if (count == 1)
            tokens[count].type = IDENTIFIER;
        else
            tokens[count].type = PARAM;

        tokens[count].text = word;
        count++;
    }
    return count;
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
    Snapshot* snapshot = new Snapshot;

    snapshot->stackDepth = callStack.depth();

    callStack.snapshot_into(
        snapshot->callStack,
        MAX_STACK_DEPTH
    );

    return snapshot;
}
Variable* findVar(Frame& fr, const string& name)
{
    for (int32_t i = 0; i < fr.argc; i++)
        if (fr.argv[i].name == name)
            return &fr.argv[i];
    for (int32_t i = 0; i < fr.localCount; i++)
        if (fr.locals[i].name == name)
            return &fr.locals[i];
    return nullptr;
}
bool setVar(Frame& fr, const string& name, int32_t value)
{
    Variable* v = findVar(fr, name);
    if (v != nullptr)
    {
        v->value = value;
        return true;
    }
    if (fr.localCount >= MAX_VARS_PER_FRAME)
        return false;
    fr.locals[fr.localCount].name = name;
    fr.locals[fr.localCount].value = value;
    fr.localCount++;
    return true;
}
int32_t getValue(Frame& fr, const string& text, bool& ok)
{
    ok = true;
    if (text.empty())
    {
        ok = false;
        return 0;
    }
    if ((text[0] >= '0' && text[0] <= '9') || text[0] == '-')
        return stoi(text);

    Variable* v = findVar(fr, text);
    if (v == nullptr)
    {
        ok = false;
        return 0;
    }
    return v->value;
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame1
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
        FILE* f = fopen(resolveBinPath, "rb");
        if (!f)
        {
            cout << "File was not found\n";
            return;
        }
        Stack<Frame> callStack;
        fseek(f, mainOffset, SEEK_SET);
        string line;
        int64_t off = readResolveRecord(f, line);
        if (off == -1)
        {
            cout << "Error: cannot read main" << endl;
            fclose(f);
            return;
        }

        Frame main_Frame;
        main_Frame.func_name = "main";
        main_Frame.argc = 0;
        main_Frame.localCount = 0;
        main_Frame.returnLine = -1;
        callStack.push(main_Frame);

        int64_t steps = 0;

        while (!callStack.isEmpty())
        {
            steps++;
            if (steps > 1000000)
            {
                cout << "Error: too many steps" << endl;
                break;
            }

            int64_t recordStart = ftell(f);

            if (readResolveRecord(f, line) == -1)
            {
                cout << "Error: program ended without func_end" << endl;
                break;
            }

            Token tokens[MAX_TOKENS];
            int32_t n = tokenizeLine(line, tokens, MAX_TOKENS);
            if (n == 0)
                continue;

            Frame& current = callStack.peek();
            string kw = tokens[0].text;

            if (kw == "set")
            {
                if (n < 3)
                {
                    cout << "Error: set needs a name and a value" << endl;
                    break;
                }
                bool ok;
                int32_t value = getValue(current, tokens[2].text, ok);
                if (!ok)
                {
                    cout << "Error: undefined variable " << tokens[2].text << endl;
                    break;
                }
                if (!setVar(current, tokens[1].text, value))
                {
                    cout << "Error: too many variables" << endl;
                    break;
                }
            }
            else if (kw == "add" || kw == "sub" || kw == "mul" || kw == "div")
            {
                if (n < 3)
                {
                    cout << "Error: " << kw << " needs 2 operands" << endl;
                    break;
                }
                Variable* dest = findVar(current, tokens[1].text);
                if (dest == nullptr)
                {
                    cout << "Error: undefined variable " << tokens[1].text << endl;
                    break;
                }
                bool ok;
                int32_t b = getValue(current, tokens[2].text, ok);
                if (!ok)
                {
                    cout << "Error: undefined variable " << tokens[2].text << endl;
                    break;
                }
                if (kw == "add")
                    dest->value = dest->value + b;
                else if (kw == "sub")
                    dest->value = dest->value - b;
                else if (kw == "mul")
                    dest->value = dest->value * b;
                else
                {
                    if (b == 0)
                    {
                        cout << "Error: division by zero" << endl;
                        break;
                    }
                    dest->value = dest->value / b;
                }
            }
            else if (kw == "call")
            {
                if (callStack.depth() >= MAX_STACK_DEPTH)
                {
                    cout << "Error: stack overflow" << endl;
                    break;
                }
                int64_t target = stoll(tokens[1].text);

                fseek(f, target, SEEK_SET);
                string funcLine;
                if (readResolveRecord(f, funcLine) == -1)
                {
                    cout << "Error: bad call target" << endl;
                    break;
                }
                Token ft[MAX_TOKENS];
                int32_t fn = tokenizeLine(funcLine, ft, MAX_TOKENS);

                if (fn - 2 != n - 2)
                {
                    cout << "Error: wrong number of arguments for " << ft[1].text << endl;
                    break;
                }

                Frame newFrame;
                newFrame.func_name = ft[1].text;
                newFrame.argc = fn - 2;
                newFrame.returnLine = (int32_t)recordStart;
                newFrame.localCount = 0;

                bool ok = true;
                for (int32_t i = 0; i < newFrame.argc; i++)
                {
                    newFrame.argv[i].name = ft[2 + i].text;
                    newFrame.argv[i].value = getValue(current, tokens[2 + i].text, ok);
                    if (!ok)
                        break;
                }
                if (!ok)
                {
                    cout << "Error: undefined argument in call" << endl;
                    break;
                }
                callStack.push(newFrame);
            }
            else if (kw == "func_end")
            {
                Frame finished = callStack.pop();
                if (callStack.isEmpty())
                    break;

                fseek(f, finished.returnLine, SEEK_SET);
                string callLine;
                readResolveRecord(f, callLine);
                Token ct[MAX_TOKENS];
                tokenizeLine(callLine, ct, MAX_TOKENS);

                Frame& caller = callStack.peek();
                for (int32_t i = 0; i < finished.argc; i++)
                {
                    string nm = ct[2 + i].text;
                    if ((nm[0] >= '0' && nm[0] <= '9') || nm[0] == '-')
                        continue;
                    setVar(caller, nm, finished.argv[i].value);
                }
            }
            else
            {
                cout << "Error: unknown instruction '" << kw << "'" << endl;
                break;
            }

            Snapshot* s = buildSnapshot(callStack);
            timeline.record(s);
        }

        fclose(f);
    }

// PASS 0x3: SERIALIZE TIMELINE
    void writeInt(FILE* f, int32_t v)
    {
        fwrite(&v, sizeof(int32_t), 1, f);
    }
    void writeString(FILE* f, const string& s)
    {
        writeInt(f, (int32_t)s.size());
        if (!s.empty())
            fwrite(s.c_str(), 1, s.size(), f);
    }
    void writeVariable(FILE* f, const Variable& v)
    {
        writeString(f, v.name);
        writeInt(f, v.value);
    }
    void writeSnapshot(FILE* f, const Snapshot& s)
    {
        writeInt(f, s.stackDepth);
        for (int32_t d = 0; d < s.stackDepth; d++)
        {
            const Frame& fr = s.callStack[d];
            writeString(f, fr.func_name);
            writeInt(f, fr.argc);
            for (int32_t i = 0; i < fr.argc; i++)
                writeVariable(f, fr.argv[i]);
            writeInt(f, fr.returnLine);
            writeInt(f, fr.localCount);
            for (int32_t i = 0; i < fr.localCount; i++)
                writeVariable(f, fr.locals[i]);
        }
    }
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
    FILE* f = fopen(tdbgPath, "wb");
    if (!f) { cout << "Error: cannot create file" << endl; return; }
    int32_t stepCount = timeline.getStepCount();
    TTDBHeader h;
    h.magic[0] = 'T'; h.magic[1] = 'T'; h.magic[2] = 'D'; h.magic[3] = 'B';
    h.version = 1;
    h.stepCount = stepCount;
    h.indexOffset = 0;
    writeHeader(f, h);

    int64_t* index = new int64_t[stepCount > 0 ? stepCount : 1];
    int32_t i = 0;
    for (TimelineNode* n = timeline.begin(); n != nullptr && i < stepCount; n = n->next, i++)
    {
        index[i] = (int64_t)ftell(f);
        writeSnapshot(f, *n->data);
    }
    h.indexOffset = (int64_t)ftell(f);
    fwrite(index, sizeof(int64_t), stepCount, f);
    delete[] index;
    fseek(f, 0, SEEK_SET);
    writeHeader(f, h);

    fclose(f);
}
// main section
bool sendAll(int sock, const char *buf, size_t n)
{
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
bool sendError(int sock, const string &message)
{
    char status = STATUS_ERROR;
    CRC32 crc;
    crc.update(&status, 1);
    crc.update(message.data(), message.size());
    char trailer[CHECKSUM_SIZE];
    encode32(crc.value(), trailer);
    return sendAll(sock, &status, 1) &&
           sendAll(sock, message.data(), message.size()) &&
           sendAll(sock, trailer, CHECKSUM_SIZE);
}
void freeTimeline(Timeline &timeline)
{
    TimelineNode *n = timeline.begin();
    while (n != nullptr)
    {
        TimelineNode *next = n->next;
        delete n->data;
        delete n;
        n = next;
    }
}
bool runPipeline(string &errorText)
{
    remove("session.tdbg");
    ostringstream captured;
    streambuf *oldBuf = cout.rdbuf(captured.rdbuf());
    bool ok = false;

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        cout << "Program is not valid";
    }
    else
    {
        int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
        if (mainOffset != -1)
        {
            Timeline timeline;
            executeProgram("resolve.bin", mainOffset, timeline);
            if (timeline.getStepCount() > 0)
            {
                writeTdbg(timeline, "session.tdbg");
                ok = true;
            }
            else if (captured.str().empty())
                cout << "Error: program produced no steps";
            freeTimeline(timeline);
        }
    }

    cout.rdbuf(oldBuf);
    errorText = captured.str();
    while (!errorText.empty() && (errorText.back() == '\n' || errorText.back() == '\r'))
        errorText.pop_back();
    if (ok && !errorText.empty())
        cerr << errorText << endl;
    if (!ok && errorText.empty())
        errorText = "program failed";
    return ok;
}
bool receiveUpload(int sock, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    char buf[IO_BUFFER_SIZE];
    TrailerChecker checker;
    bool gotStatus = false;
    char status = 0;
    uint64_t total = 0;
    bool ok = true;
    while (true)
    {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (n == 0)
            break;
        const char *p = buf;
        size_t len = (size_t)n;
        if (!gotStatus)
        {
            status = p[0];
            checker.addPrefixByte(status);
            gotStatus = true;
            p++;
            len--;
        }
        string safe = checker.feed(p, len);
        total += safe.size();
        if (total > MAX_SOURCE_BYTES)
        {
            ok = false;
            break;
        }
        if (!safe.empty())
            fwrite(safe.data(), 1, safe.size(), f);
    }
    fclose(f);
    if (!ok || !gotStatus || status != STATUS_OK)
        return false;
    return checker.verify();
}
bool sendTdbg(int sock, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return sendError(sock, "server could not open the .tdbg file");
    char status = STATUS_OK;
    CRC32 crc;
    crc.update(&status, 1);
    bool ok = sendAll(sock, &status, 1);
    char buf[IO_BUFFER_SIZE];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        crc.update(buf, n);
        ok = sendAll(sock, buf, n);
    }
    fclose(f);
    if (!ok)
        return false;
    char trailer[CHECKSUM_SIZE];
    encode32(crc.value(), trailer);
    return sendAll(sock, trailer, CHECKSUM_SIZE);
}
void handleClient(int sock)
{
    timeval tv;
    tv.tv_sec = SOCKET_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!receiveUpload(sock, "source.bin"))
    {
        sendError(sock, "upload checksum mismatch");
        return;
    }
    string errorText;
    if (!runPipeline(errorText))
    {
        sendError(sock, errorText);
        return;
    }
    sendTdbg(sock, "session.tdbg");
}
int main(int argc, char *argv[])
{
    int port = 5001;
    if (argc >= 2)
        port = atoi(argv[1]);

    int listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock < 0)
    {
        perror("socket");
        return 1;
    }
    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listenSock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }
    if (listen(listenSock, 16) < 0)
    {
        perror("listen");
        return 1;
    }
    while (true)
    {
        int cs = accept(listenSock, nullptr, nullptr);
        if (cs < 0)
        {
            if (errno != EINTR)
                perror("accept");
            continue;
        }
        handleClient(cs);
        close(cs);
    }
    return 0;
}