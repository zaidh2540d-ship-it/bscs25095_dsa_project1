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
//#include <unistd.h>
//#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
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
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



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
    fread(&offset, sizeof(int64_t), 1, f);
    int32_t size;
    fread(&size, sizeof(int32_t), 1, f);
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
            toWrite = line.substr(0, p) + "0x00000000" + line.substr(p + target.size());
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
        snprintf(buf, sizeof(buf), "0x%08llX", (unsigned long long)targetOffset);
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
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame1
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

//// PASS 0x3: SERIALIZE TIMELINE
//void writeTdbg(Timeline &timeline, const char *tdbgPath)
//{
//    // placeholder for header
//    // index array of the size of stepcount from the timeline
//    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
//    // after timeline add the index array i the file
//    // update the header
//}
//// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        cout << "Program is not valid";
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    //executeProgram("resolve.bin", mainOffset, timeline);

    //writeTdbg(timeline, "session.tdbg");
    return 0;
}