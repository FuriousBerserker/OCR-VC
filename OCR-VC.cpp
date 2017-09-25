/**
 * A implementation of FastTrack for Open Community Runtime (OCR) programs
 */

#include <stdio.h>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include "ocr-types.h"
#include "pin.H"

#define DEBUG 0
#define INSTRUMENT 1
#define DETECT_RACE 1
#define MEASURE_TIME 1

#define DEFAULT_EPOCH 0
#define START_EPOCH 1
using namespace ::std;

class GuidComparator {
   public:
    bool operator()(const ocrGuid_t& key1, const ocrGuid_t& key2) const;
};

class Metadata {
   public:
    enum ObjectType { EDT, DB, EVENT };
    list<ocrGuid_t> controlDependences;
    ObjectType type;
    u64 subscribes;
    Metadata(ObjectType type);
    bool isEdt();
    bool isDB();
    bool isEvent();
    //    void addControlDependence(ocrGuid_t& guid);
    bool hasControlDependence();
    bool hasSubscribe();
};

class VC {
   public:
    // vector clock representation
    map<ocrGuid_t, u64, GuidComparator> clockMap;
    bool compress;
    // epoch representation
    ocrGuid_t guid;
    u64 epoch;
    VC(bool epochIndicator);
    VC(VC& vc);
    bool isCompressed() const;
    bool isEmpty() const;
    bool isEpoch() const;
    friend bool operator<=(const VC& vc1, const VC& vc2);
    void increment(ocrGuid_t& guid);
    void merge(VC& vc);
    void updateEpoch(ocrGuid_t& guid, u64 epoch, bool isOverride);
    void clear();
    u64 search(const ocrGuid_t& key) const;
    string toString() const;
};

class BytePage {
   public:
    VC write;
    VC read;
    ADDRINT writeOp;
    map<ocrGuid_t, ADDRINT, GuidComparator> readOp;
    BytePage();
    bool hasWrite();
    bool hasRead();
    void update(ocrGuid_t& guid, VC* vc, ADDRINT op, bool isRead);
};

class DBPage {
   public:
    uintptr_t startAddress;
    u64 length;
    BytePage** bytePageArray;
    DBPage(uintptr_t addr, u64 len);
    void updateBytePages(ocrGuid_t& guid, VC* vc, ADDRINT op, uintptr_t addr,
                         u64 len, bool isRead);
    BytePage* getBytePage(uintptr_t addr);
    int compareAddr(uintptr_t addr);
};

class ThreadLocalStore {
   public:
    ocrGuid_t edtID;
    vector<DBPage*> acquiredDB;
    void initializeAcquiredDB(u32 dpec, ocrEdtDep_t* depv);
    void insertDB(ocrGuid_t& guid);
    DBPage* getDB(uintptr_t addr);
    void removeDB(DBPage* dbPage);

   private:
    bool searchDB(uintptr_t addr, DBPage** ptr, u64* offset);
};

// measure time
#if MEASURE_TIME
clock_t program_start, program_end;
#endif

// metadata map
map<ocrGuid_t, Metadata*, GuidComparator> dpMap;

// vector clock map
map<ocrGuid_t, VC*, GuidComparator> vcMap;

// EDT acquired DB
map<intptr_t, DBPage*> dbMap;

// library list
vector<string> skippedLibraries;

// user code image name
string userCodeImg;

// thread local information
ThreadLocalStore tls;

unsigned res_time = 0;

int usage() {
    cout << "This tool detects data race by vector clock for OCR program"
         << endl;
    return -1;
}

/**
 * Test whether guid id NULL_GUID
 */
bool isNullGuid(ocrGuid_t guid) {
    if (guid.guid == NULL_GUID.guid) {
        return true;
    } else {
        return false;
    }
}

/**
 * Test whether s2 is the suffix of s1
 */
bool isEndWith(std::string& s1, std::string& s2) {
    if (s1.size() < s2.size()) {
        return false;
    } else {
        return !s1.compare(s1.size() - s2.size(), std::string::npos, s2);
    }
}

bool isSkippedLibrary(IMG img) {
    string imageName = IMG_Name(img);
    for (vector<string>::iterator li = skippedLibraries.begin(),
                                  le = skippedLibraries.end();
         li != le; li++) {
        if (isEndWith(imageName, *li)) {
            return true;
        }
    }
    return false;
}

bool isOCRLibrary(IMG img) {
    string ocrLibraryName = "libocr_x86.so";
    string imageName = IMG_Name(img);
    if (isEndWith(imageName, ocrLibraryName)) {
        return true;
    } else {
        return false;
    }
}

bool isUserCodeImg(IMG img) {
    string imageName = IMG_Name(img);
    if (imageName == userCodeImg) {
        return true;
    } else {
        return false;
    }
}

bool isIgnorableIns(INS ins) {
    if (INS_IsStackRead(ins) || INS_IsStackWrite(ins)) return true;

    // skip call, ret and JMP instructions
    if (INS_IsBranchOrCall(ins) || INS_IsRet(ins)) {
        return true;
    }

    return false;
}

bool GuidComparator::operator()(const ocrGuid_t& key1,
                                const ocrGuid_t& key2) const {
    return key1.guid < key2.guid ? true : false;
}

Metadata::Metadata(ObjectType type) : type(type), subscribes(0) {}

inline bool Metadata::isEdt() { return this->type == EDT; }

inline bool Metadata::isDB() { return this->type == DB; }

inline bool Metadata::isEvent() { return this->type == EVENT; }
// inline void Metadata::addControlDependence(ocrGuid_t& guid) {
//    this->controlDependences.push_back(guid);
//}

inline bool Metadata::hasControlDependence() {
    return !this->controlDependences.empty();
}

inline bool Metadata::hasSubscribe() { return this->subscribes != 0; }

inline void addControlDependence(ocrGuid_t& src, ocrGuid_t& dst) {
    dpMap[dst]->controlDependences.push_back(src);
    dpMap[src]->subscribes++;
}

inline void removeOCROBject(ocrGuid_t& guid) {
    delete dpMap[guid];
    dpMap.erase(guid);
    delete vcMap[guid];
    vcMap.erase(guid);
}
inline void cleanSubscribe(ocrGuid_t& src) {
    dpMap[src]->subscribes--;
    if (dpMap[src]->subscribes == 0) {
        removeOCROBject(src);
    }
}

VC::VC(bool compress) : compress(compress), epoch(DEFAULT_EPOCH) {
    this->guid.guid = NULL_GUID.guid;
}

VC::VC(VC& vc) : clockMap(vc.clockMap), compress(vc.compress), epoch(vc.epoch) {
    this->guid.guid = vc.guid.guid;
}

inline bool VC::isCompressed() const { return this->compress; }

inline bool VC::isEmpty() const {
    return isNullGuid(this->guid) && this->clockMap.empty();
}

inline bool VC::isEpoch() const {
    assert(this->isCompressed());
    return this->clockMap.empty();
}

void VC::increment(ocrGuid_t& guid) {
    assert(!this->isCompressed());
    map<ocrGuid_t, u64>::iterator ci = clockMap.find(guid);
    if (ci == clockMap.end()) {
        clockMap.insert(make_pair(guid, START_EPOCH));
    } else {
        ci->second++;
    }
}

void VC::merge(VC& vc) {
    assert(!this->isCompressed() && !vc.isCompressed());
    for (map<ocrGuid_t, u64>::iterator ci2 = vc.clockMap.begin(),
                                       ce = vc.clockMap.end();
         ci2 != ce; ci2++) {
        map<ocrGuid_t, u64>::iterator ci = this->clockMap.find(ci2->first);
        if (ci == this->clockMap.end()) {
            this->clockMap.insert(make_pair(ci2->first, ci2->second));
        } else if (ci->second < ci2->second) {
            ci->second = ci2->second;
        }
    }
}

void VC::updateEpoch(ocrGuid_t& guid, u64 epoch, bool isOverride) {
    assert(this->isCompressed());
    if (isOverride) {
        this->guid.guid = guid.guid;
        this->epoch = epoch;
    } else {
        if (this->isEpoch()) {
            if (isNullGuid(this->guid) || this->guid.guid == guid.guid) {
                this->guid.guid = guid.guid;
                this->epoch = epoch;
            } else {
                this->clockMap.insert(make_pair(this->guid, this->epoch));
                this->clockMap.insert(make_pair(guid, epoch));
            }
        } else {
            this->clockMap[guid] = epoch;
        }
    }
}

inline void VC::clear() {
    clockMap.clear();
    guid.guid = NULL_GUID.guid;
    epoch = DEFAULT_EPOCH;
}

inline u64 VC::search(const ocrGuid_t& key) const {
    map<ocrGuid_t, u64>::const_iterator ci = this->clockMap.find(key);
    if (ci == this->clockMap.end()) {
        return DEFAULT_EPOCH;
    } else {
        return ci->second;
    }
}

string VC::toString() const {
    stringstream ss;
    if (this->isCompressed()) {
        if (this->isEpoch()) {
            ss << hex << this->guid.guid << dec;
            ss << " -> ";
            ss << this->epoch;
            ss << '\n';
        } else {
            for (map<ocrGuid_t, u64>::const_iterator
                     ci = this->clockMap.begin(),
                     ce = this->clockMap.end();
                 ci != ce; ci++) {
                ss << hex << ci->first.guid << dec;
                ss << " -> ";
                ss << ci->second;
                ss << '\n';
            }
        }
    } else {
        for (map<ocrGuid_t, u64>::const_iterator ci = this->clockMap.begin(),
                                                 ce = this->clockMap.end();
             ci != ce; ci++) {
            ss << hex << ci->first.guid << dec;
            ss << " -> ";
            ss << ci->second;
            ss << '\n';
        }
    }
    return ss.str();
}

inline bool operator<=(const VC& vc1, const VC& vc2) {
    assert(vc1.isCompressed() && !vc2.isCompressed());
    //    cout << vc1.toString() << endl;
    //    cout << vc2.toString() << endl;
    bool result = true;
    if (vc1.isEpoch()) {
        u64 epoch = vc2.search(vc1.guid);
        if (vc1.epoch <= epoch) {
            result = true;
        } else {
            result = false;
        }
    } else {
        for (map<ocrGuid_t, u64>::const_iterator ci = vc1.clockMap.begin(),
                                                 ce = vc1.clockMap.end();
             ci != ce; ci++) {
            u64 epoch = vc2.search(ci->first);
            if (ci->second > epoch) {
                result = false;
                break;
            }
        }
    }
    return result;
}

inline void selfIncrement(ocrGuid_t& guid) { vcMap[guid]->increment(guid); }

BytePage::BytePage() : write(true), read(true) {}

bool BytePage::hasWrite() {
    if (!write.isEmpty()) {
        return true;
    } else {
        return false;
    }
}

bool BytePage::hasRead() {
    if (!read.isEmpty()) {
        return true;
    } else {
        return false;
    }
}

void BytePage::update(ocrGuid_t& guid, VC* vc, ADDRINT op, bool isRead) {
    if (isRead) {
        read.updateEpoch(guid, vc->search(guid), false);
        readOp.insert(make_pair(guid, op));
    } else {
        read.clear();
        readOp.clear();
        write.updateEpoch(guid, vc->search(guid), true);
        writeOp = op;
    }
}

DBPage::DBPage(uintptr_t addr, u64 len) : startAddress(addr), length(len) {
    bytePageArray = new BytePage*[len];
    memset(bytePageArray, 0, sizeof(uintptr_t) * len);
}

void DBPage::updateBytePages(ocrGuid_t& guid, VC* vc, ADDRINT op,
                             uintptr_t addr, u64 len, bool isRead) {
    assert(addr >= startAddress && addr + len <= startAddress + length);
    uintptr_t offset = addr - startAddress;
    for (u64 i = 0; i < len; i++) {
        if (!bytePageArray[offset + i]) {
            bytePageArray[offset + i] = new BytePage();
        }
        bytePageArray[offset + i]->update(guid, vc, op, isRead);
    }
}

BytePage* DBPage::getBytePage(uintptr_t addr) {
    assert(addr >= startAddress && addr < startAddress + length);
    return bytePageArray[addr - startAddress];
}

int DBPage::compareAddr(uintptr_t addr) {
    if (addr < startAddress) {
        return 1;
    } else if (addr < startAddress + length) {
        return 0;
    } else {
        return -1;
    }
}

bool compareDB(DBPage* db1, DBPage* db2) {
    return db1->startAddress < db2->startAddress;
}

bool ThreadLocalStore::searchDB(uintptr_t addr, DBPage** ptr, u64* offset) {
#if DEBUG
    cout << "search DB\n";
#endif
    int start = 0;
    int end = acquiredDB.size() - 1;
    while (start <= end) {
        int middle = (start + end) / 2;
        DBPage* dbPage = acquiredDB[middle];
        int res = dbPage->compareAddr(addr);
        if (res == -1) {
            start = middle + 1;
        } else if (res == 1) {
            end = middle - 1;
        } else {
            if (ptr) {
                *ptr = acquiredDB[middle];
            }
            if (offset) {
                *offset = middle;
            }
#if DEBUG
            cout << "search DB finish, true\n";
#endif
            return true;
        }
    }
    if (ptr) {
        *ptr = NULL;
    }
    if (offset) {
        *offset = start;
    }
#if DEBUG
    cout << "search DB finish, false\n";
#endif
    return false;
}

void ThreadLocalStore::initializeAcquiredDB(u32 depc, ocrEdtDep_t* depv) {
    acquiredDB.clear();
    acquiredDB.reserve(2 * depc);
    for (u32 i = 0; i < depc; i++) {
        if (depv[i].ptr) {
            acquiredDB.push_back(dbMap[depv[i].guid.guid]);
            assert(dbMap[depv[i].guid.guid]->startAddress ==
                   (uintptr_t)depv[i].ptr);
        }
    }
    sort(acquiredDB.begin(), acquiredDB.end(), compareDB);
}

void ThreadLocalStore::insertDB(ocrGuid_t& guid) {
    DBPage* dbPage = dbMap[guid.guid];
    u64 offset;
    bool isReallocated = searchDB(dbPage->startAddress, NULL, &offset);
    assert(!isReallocated);
    acquiredDB.insert(acquiredDB.begin() + offset, dbPage);
}

DBPage* ThreadLocalStore::getDB(uintptr_t addr) {
    DBPage* dbPage;
    searchDB(addr, &dbPage, NULL);
    return dbPage;
}

void ThreadLocalStore::removeDB(DBPage* dbPage) {
    u64 offset;
    bool isContain = searchDB(dbPage->startAddress, NULL, &offset);
    if (isContain) {
        acquiredDB.erase(acquiredDB.begin() + offset);
    }
}

// inline ocrGuid_t* getEdtGuid(THREADID tid) {
//    ThreadLocalStore* data =
//        static_cast<ThreadLocalStore*>(PIN_GetThreadData(tlsKey, tid));
//    return &data->edtGuid;
//}

inline void initializeTLS(ocrGuid_t& edtGuid, u32 depc, ocrEdtDep_t* depv,
                          THREADID tid) {
    tls.edtID.guid = edtGuid.guid;
    tls.initializeAcquiredDB(depc, depv);
}
// void argsMainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
//#if DEBUG
//    cout << "argsMainEdt" << endl;
//#endif
//    ocrGuid_t* depIdv = new ocrGuid_t[depc];
//    for (uint32_t i = 0; i < depc; i++) {
//        depIdv[i] = depv[i].guid;
//    }
//    ocrGuid_t mainEdtGuid = {0};
//    Node* mainEdtNode = new Node(mainEdtGuid, depc, depIdv, Node::EDT);
//    computationGraph[mainEdtNode->id] = mainEdtNode;
//    delete[] depIdv;
//}

/**
 *depv is always NULL
 */
void afterEdtCreate(ocrGuid_t guid, ocrGuid_t templateGuid, u32 paramc,
                    u64* paramv, u32 depc, ocrGuid_t* depv, u16 properties,
                    ocrGuid_t outputEvent, ocrGuid_t parent) {
#if DEBUG
    cout << "afterEdtCreate" << endl;
#endif
    if (depc >= 0xFFFFFFFE) {
        cerr << "error" << endl;
        exit(0);
    }
    if (!isNullGuid(parent)) {
        VC* parentVC = vcMap[parent];
        VC* childVC = new VC(*parentVC);
        selfIncrement(parent);
        vcMap[guid] = childVC;
    } else {
        VC* childVC = new VC(false);
        vcMap[guid] = childVC;
    }

    if (!isNullGuid(outputEvent)) {
        vcMap[outputEvent] = new VC(false);
    }

    dpMap[guid] = new Metadata(Metadata::EDT);

    if (!isNullGuid(outputEvent)) {
        dpMap[outputEvent] = new Metadata(Metadata::EVENT);
        addControlDependence(guid, outputEvent);
    }
#if DEBUG
    cout << "afterEdtCreate finish" << endl;
#endif
}

void afterDbCreate(ocrGuid_t guid, void* addr, u64 len, u16 flags,
                   ocrInDbAllocator_t allocator) {
#if DEBUG
    cout << "afterDbCreate" << endl;
#endif
    DBPage* dbPage = new DBPage((uintptr_t)addr, len);
    dbMap[guid.guid] = dbPage;

    // new created DB is acquired by current EDT instantly
    tls.insertDB(guid);

    Metadata* metadata = new Metadata(Metadata::DB);
    dpMap[guid] = metadata;
#if DEBUG
    cout << "afterDbCreate finish" << endl;
#endif
}

void afterEventCreate(ocrGuid_t guid, ocrEventTypes_t eventType,
                      u16 properties) {
#if DEBUG
    cout << "afterEventCreate" << endl;
#endif
    VC* childVC = new VC(false);
    vcMap[guid] = childVC;

    Metadata* metadata = new Metadata(Metadata::EVENT);
    dpMap[guid] = metadata;
#if DEBUG
    cout << "afterEventCreate finish" << endl;
#endif
}

void afterAddDependence(ocrGuid_t source, ocrGuid_t destination, u32 slot,
                        ocrDbAccessMode_t mode) {
#if DEBUG
    cout << "afterAddDependence" << endl;
#endif

    if (!isNullGuid(source)) {
        assert(dpMap[source]);
        assert(dpMap[destination]);
        if (!dpMap[source]->isDB()) {
            addControlDependence(source, destination);
        }
    }
#if DEBUG
    cout << "afterAddDependence finish" << endl;
#endif
}

void afterEventSatisfy(ocrGuid_t edtGuid, ocrGuid_t eventGuid,
                       ocrGuid_t dataGuid, u32 slot) {
#if DEBUG
    cout << "afterEventSatisfy" << endl;
#endif
    addControlDependence(edtGuid, eventGuid);
#if DEBUG
    cout << "afterEventSatisfy finish" << endl;
#endif
}

void preEdt(THREADID tid, ocrGuid_t edtGuid, u32 paramc, u64* paramv, u32 depc,
            ocrEdtDep_t* depv, u64* dbSizev) {
#if DEBUG
    cout << "preEdt" << endl;
#endif
    Metadata* metadata = dpMap[edtGuid];
    VC* vc = vcMap[edtGuid];
    for (list<ocrGuid_t>::iterator gi = metadata->controlDependences.begin(),
                                   ge = metadata->controlDependences.end();
         gi != ge; gi++) {
        vc->merge(*vcMap[*gi]);
        cleanSubscribe(*gi);
        //         cout << "merge edt guid = " << hex << gi->guid << endl;
        //        cout << vcMap[*gi]->toString() << endl;
    }
    selfIncrement(edtGuid);
    //    cout << "edt guid = " << hex << edtGuid.guid << endl;
    //    cout << vc->toString() << endl;
    initializeTLS(edtGuid, depc, depv, tid);
#if DEBUG
    cout << "preEdt finish" << endl;
#endif
}

void afterDbDestroy(ocrGuid_t dbGuid) {
#if DEBUG
    cout << "afterDbDestroy" << endl;
#endif
    map<intptr_t, DBPage*>::iterator di = dbMap.find(dbGuid.guid);
    if (di != dbMap.end()) {
        DBPage* dbPage = di->second;
        tls.removeDB(dbPage);
        dbMap.erase(dbGuid.guid);
        delete dbPage;
    }
#if DEBUG
    cout << "afterDbDestroy finish" << endl;
#endif
}

void afterEventPropagate(ocrGuid_t eventGuid) {
#if DEBUG
    cout << "afterEventPropagate" << endl;
#endif
    Metadata* metadata = dpMap[eventGuid];
    VC* vc = vcMap[eventGuid];
    if (metadata->hasControlDependence()) {
        for (list<ocrGuid_t>::iterator
                 gi = metadata->controlDependences.begin(),
                 ge = metadata->controlDependences.end();
             gi != ge; gi++) {
            vc->merge(*vcMap[*gi]);
            cleanSubscribe(*gi);
        }
    }
#if DEBUG
    cout << "afterEventPropagate finish" << endl;
#endif
}

void afterEdtTerminate(ocrGuid_t edtGuid) {
#if DEBUG
    cout << "afterEdtTerminate" << endl;
#endif
    map<ocrGuid_t, Metadata*>::iterator mi = dpMap.find(edtGuid);
    if (mi != dpMap.end() && mi->second->hasSubscribe()) {
        removeOCROBject(edtGuid);
    }
#if DEBUG
    cout << "afterEdtTerminate finish" << endl;
#endif
}

void fini(int32_t code, void* v) {
#if DEBUG
    cout << "fini" << endl;
#endif

#if MEASURE_TIME
    program_end = clock();
    double time_span = program_end - program_start;
    time_span /= CLOCKS_PER_SEC;
    cout << "elapsed time: " << time_span << " seconds" << endl;
#endif
}

void overload(IMG img, void* v) {
#if DEBUG
    cout << "img: " << IMG_Name(img) << endl;
#endif

    if (isOCRLibrary(img)) {
        // monitor mainEdt
        //        RTN mainEdtRTN = RTN_FindByName(img, "mainEdt");
        //        if (RTN_Valid(mainEdtRTN)) {
        //#if DEBUG
        //            cout << "instrument mainEdt" << endl;
        //#endif
        //            RTN_Open(mainEdtRTN);
        //            RTN_InsertCall(mainEdtRTN, IPOINT_BEFORE,
        //            (AFUNPTR)argsMainEdt,
        //                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
        //                           IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
        //                           IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
        //                           IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
        //                           IARG_END);
        //            RTN_Close(mainEdtRTN);
        //        }

        // replace notifyEdtCreate
        RTN rtn = RTN_FindByName(img, "notifyEdtCreate");
        if (RTN_Valid(rtn)) {
#if DEBUG
            cout << "replace notifyEdtCreate" << endl;
#endif
            PROTO proto_notifyEdtCreate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEdtCreate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG(u32), PIN_PARG(u64*), PIN_PARG(u32),
                PIN_PARG(ocrGuid_t*), PIN_PARG(u16),
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG_END());
            RTN_ReplaceSignature(
                rtn, AFUNPTR(afterEdtCreate), IARG_PROTOTYPE,
                proto_notifyEdtCreate, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_FUNCARG_ENTRYPOINT_VALUE,
                2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 4, IARG_FUNCARG_ENTRYPOINT_VALUE,
                5, IARG_FUNCARG_ENTRYPOINT_VALUE, 6,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 7, IARG_FUNCARG_ENTRYPOINT_VALUE,
                8, IARG_END);
            PROTO_Free(proto_notifyEdtCreate);
        }

        // replace notidyDbCreate
        rtn = RTN_FindByName(img, "notifyDbCreate");
        if (RTN_Valid(rtn)) {
#if DEBUG
            cout << "replace notifyDbCreate" << endl;
#endif
            PROTO proto_notifyDbCreate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyDbCreate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG(void*), PIN_PARG(u64),
                PIN_PARG(u16), PIN_PARG_ENUM(ocrInDbAllocator_t),
                PIN_PARG_END());
            RTN_ReplaceSignature(
                rtn, AFUNPTR(afterDbCreate), IARG_PROTOTYPE,
                proto_notifyDbCreate, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_FUNCARG_ENTRYPOINT_VALUE,
                2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 4, IARG_END);
            PROTO_Free(proto_notifyDbCreate);
        }

        // replace notifyEventCreate
        rtn = RTN_FindByName(img, "notifyEventCreate");
        if (RTN_Valid(rtn)) {
#if DEBUG
            cout << "replace notifyEventCreate" << endl;
#endif
            PROTO proto_notifyEventCreate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEventCreate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_ENUM(ocrEventTypes_t),
                PIN_PARG(u16), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterEventCreate), IARG_PROTOTYPE,
                                 proto_notifyEventCreate,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 2, IARG_END);
            PROTO_Free(proto_notifyEventCreate);
        }

        // replace notifyAddDependence
        rtn = RTN_FindByName(img, "notifyAddDependence");
        if (RTN_Valid(rtn)) {
#if DEBUG
            cout << "replace notifyAddDependence" << endl;
#endif
            PROTO proto_notifyAddDependence = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyAddDependence",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG(u32), PIN_PARG_ENUM(ocrDbAccessMode_t),
                PIN_PARG_END());
            RTN_ReplaceSignature(
                rtn, AFUNPTR(afterAddDependence), IARG_PROTOTYPE,
                proto_notifyAddDependence, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_FUNCARG_ENTRYPOINT_VALUE,
                2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3, IARG_END);
            PROTO_Free(proto_notifyAddDependence);
        }

        // replace notifyEventSatisfy
        rtn = RTN_FindByName(img, "notifyEventSatisfy");
        if (RTN_Valid(rtn)) {
#if DEBUG
            cout << "replace notifyEventSatisfy" << endl;
#endif
            PROTO proto_notifyEventSatisfy = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEventSatisfy",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_AGGREGATE(ocrGuid_t),
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG(u32), PIN_PARG_END());
            RTN_ReplaceSignature(
                rtn, AFUNPTR(afterEventSatisfy), IARG_PROTOTYPE,
                proto_notifyEventSatisfy, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_FUNCARG_ENTRYPOINT_VALUE,
                2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3, IARG_END);
            PROTO_Free(proto_notifyEventSatisfy);
        }

        // replace notifyShutdown
        //        rtn = RTN_FindByName(img, "notifyShutdown");
        //        if (RTN_Valid(rtn)) {
        //#if DEBUG
        //            cout << "replace notifyShutdown" << endl;
        //#endif
        //            PROTO proto_notifyShutdown =
        //                PROTO_Allocate(PIN_PARG(void), CALLINGSTD_DEFAULT,
        //                               "notifyShutdown", PIN_PARG_END());
        //            RTN_ReplaceSignature(rtn, AFUNPTR(fini), IARG_PROTOTYPE,
        //                                 proto_notifyShutdown, IARG_END);
        //            PROTO_Free(proto_notifyShutdown);
        //        }

        // replace notifyEdtStart
        rtn = RTN_FindByName(img, "notifyEdtStart");
        if (RTN_Valid(rtn)) {
#if DEBUG
            cout << "replace notifyEdtStart" << endl;
#endif
            PROTO proto_notifyEdtStart = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEdtStart",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG(u32), PIN_PARG(u64*),
                PIN_PARG(u32), PIN_PARG(ocrEdtDep_t*), PIN_PARG(u64*),
                PIN_PARG_END());
            RTN_ReplaceSignature(
                rtn, AFUNPTR(preEdt), IARG_PROTOTYPE, proto_notifyEdtStart,
                IARG_THREAD_ID, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_FUNCARG_ENTRYPOINT_VALUE,
                2, IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
                IARG_FUNCARG_ENTRYPOINT_VALUE, 4, IARG_FUNCARG_ENTRYPOINT_VALUE,
                5, IARG_END);
            PROTO_Free(proto_notifyEdtStart);
        }

        // replace notifyDBDestroy
        rtn = RTN_FindByName(img, "notifyDbDestroy");
        if (RTN_Valid(rtn)) {
#if DEBUG
            cout << "replace notifyDbDestroy" << endl;
#endif
            PROTO proto_notifyDbDestroy = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyDbDestroy",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterDbDestroy), IARG_PROTOTYPE,
                                 proto_notifyDbDestroy,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
            PROTO_Free(proto_notifyDbDestroy);
        }

        // replace notifyEventPropagate
        rtn = RTN_FindByName(img, "notifyEventPropagate");
        if (RTN_Valid(rtn)) {
#if DEBUG
            cout << "replace notifyEventPropagate" << endl;
#endif
            PROTO proto_notifyEventPropagate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEventPropagate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterEventPropagate),
                                 IARG_PROTOTYPE, proto_notifyEventPropagate,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
            PROTO_Free(proto_notifyEventPropagate);
        }

        // replace notifyEdtTerminate
        rtn = RTN_FindByName(img, "notifyEdtTerminate");
        if (RTN_Valid(rtn)) {
#if DEBUG
            cout << "replace notifyEdtTerminate";
            PROTO proto_notifyEdtTerminate = PROTO_Allocate(
                PIN_PARG(void), CALLINGSTD_DEFAULT, "notifyEdtTerminate",
                PIN_PARG_AGGREGATE(ocrGuid_t), PIN_PARG_END());
            RTN_ReplaceSignature(rtn, AFUNPTR(afterEdtTerminate),
                                 IARG_PROTOTYPE, proto_notifyEdtTerminate,
                                 IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
            PROTO_Free(proto_notifyEdtTerminate);
#endif
        }
    }
}

/**
 * Output detail of a data race
 */
void outputRaceInfo(ADDRINT ip1, bool ip1IsRead, ADDRINT ip2, bool ip2IsRead) {
    int32_t ip1Line, ip1Column, ip2Line, ip2Column;
    string ip1File, ip2File;
    string ip1Type, ip2Type;
    if (ip1IsRead) {
        ip1Type = "Read";
    } else {
        ip1Type = "Write";
    }
    if (ip2IsRead) {
        ip2Type = "Read";
    } else {
        ip2Type = "Write";
    }

    PIN_LockClient();
    PIN_GetSourceLocation(ip1, &ip1Column, &ip1Line, &ip1File);
    PIN_GetSourceLocation(ip2, &ip2Column, &ip2Line, &ip2File);
    PIN_UnlockClient();

    cout << ip1Type << "-" << ip2Type << " race detect!" << endl;
    cout << "first op is " << ip1 << " in " << ip1File << ": " << ip1Line
         << ": " << ip1Column << endl;
    cout << "second op is " << ip2 << " in " << ip2File << ": " << ip2Line
         << ": " << ip2Column << endl;
}

void checkDataRace(ADDRINT ip, VC* vc, bool isRead, BytePage* bytePage,
                   uintptr_t addr) {
    if (isRead) {
        if (bytePage->hasWrite()) {
            if (!(bytePage->write <= *vc)) {
                outputRaceInfo(bytePage->writeOp, false, ip, true);
                abort();
            }
        }
    } else {
        if (bytePage->hasWrite()) {
            if (!(bytePage->write <= *vc)) {
                outputRaceInfo(bytePage->writeOp, false, ip, false);
                abort();
            }
        }
        if (bytePage->hasRead()) {
            if (!(bytePage->read <= *vc)) {
                for (map<ocrGuid_t, ADDRINT>::iterator
                         pi = bytePage->readOp.begin(),
                         pe = bytePage->readOp.end();
                     pi != pe; pi++) {
                    outputRaceInfo(pi->second, true, ip, false);
                }
                abort();
            }
        }
    }
}

void recordMemRead(void* addr, uint32_t size, ADDRINT sp, ADDRINT ip) {
#if DEBUG
    cout << "record memory read\n";
#endif
    if (!isNullGuid(tls.edtID)) {
        DBPage* dbPage = tls.getDB((uintptr_t)addr);
        if (dbPage) {
#if DETECT_RACE
            for (uint32_t i = 0; i < size; i++) {
                BytePage* current = dbPage->getBytePage((uintptr_t)addr + i);
                if (current) {
                    checkDataRace(ip, vcMap[tls.edtID], true, current,
                                  (uintptr_t)addr + i);
                }
            }
#endif
            dbPage->updateBytePages(tls.edtID, vcMap[tls.edtID], ip,
                                    (uintptr_t)addr, size, true);
        }
    }
#if DEBUG
    cout << "record memory read finish\n";
#endif
}

void recordMemWrite(void* addr, uint32_t size, ADDRINT sp, ADDRINT ip) {
#if DEBUG
    cout << "record memory write\n";
#endif
    if (!isNullGuid(tls.edtID)) {
        DBPage* dbPage = tls.getDB((uintptr_t)addr);
        if (dbPage) {
#if DETECT_RACE
            for (uint32_t i = 0; i < size; i++) {
                BytePage* current = dbPage->getBytePage((uintptr_t)addr + i);
                if (current) {
                    checkDataRace(ip, vcMap[tls.edtID], false, current,
                                  (uintptr_t)addr + i);
                }
            }
#endif
            dbPage->updateBytePages(tls.edtID, vcMap[tls.edtID], ip,
                                    (uintptr_t)addr, size, false);
        }
    }
#if DEBUG
    cout << "record memory write finish\n";
#endif
}

void instrumentInstruction(INS ins) {
    if (isIgnorableIns(ins)) return;

    if (INS_IsAtomicUpdate(ins)) return;

    uint32_t memOperands = INS_MemoryOperandCount(ins);

    // Iterate over each memory operand of the instruction.
    for (uint32_t memOp = 0; memOp < memOperands; memOp++) {
        if (INS_MemoryOperandIsRead(ins, memOp)) {
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)recordMemRead,
                                     IARG_MEMORYOP_EA, memOp,
                                     IARG_MEMORYREAD_SIZE, IARG_REG_VALUE,
                                     REG_STACK_PTR, IARG_INST_PTR, IARG_END);
        }
        // Note that in some architectures a single memory operand can be
        // both read and written (for instance incl (%eax) on IA-32)
        // In that case we instrument it once for read and once for write.
        if (INS_MemoryOperandIsWritten(ins, memOp)) {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)recordMemWrite, IARG_MEMORYOP_EA,
                memOp, IARG_MEMORYWRITE_SIZE, IARG_REG_VALUE, REG_STACK_PTR,
                IARG_INST_PTR, IARG_END);
        }
    }
}

void instrumentRoutine(RTN rtn) {
    RTN_Open(rtn);
    for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
        instrumentInstruction(ins);
    }
    RTN_Close(rtn);
}

void instrumentImage(IMG img, void* v) {
#if DEBUG
    cout << "instrument image\n";
#endif
    if (isUserCodeImg(img)) {
        for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec)) {
            for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn);
                 rtn = RTN_Next(rtn)) {
                instrumentRoutine(rtn);
            }
        }
    }
#if DEBUG
    cout << "instrument image finish\n";
#endif
}

void initSkippedLibrary() {
    skippedLibraries.push_back("ld-linux-x86-64.so.2");
    skippedLibraries.push_back("libpthread.so.0");
    skippedLibraries.push_back("libc.so.6");
    skippedLibraries.push_back("libocr_x86.so");
}

void init() { initSkippedLibrary(); }

int main(int argc, char* argv[]) {
#if MEASURE_TIME
    program_start = clock();
#endif

    PIN_InitSymbols();
    if (PIN_Init(argc, argv)) {
        return usage();
    }
    int argi;
    for (argi = 0; argi < argc; argi++) {
        string arg = argv[argi];
        if (arg == "--") {
            break;
        }
    }
    userCodeImg = argv[argi + 1];
    cout << "User image is " << userCodeImg << endl;
    IMG_AddInstrumentFunction(overload, 0);
#if INSTRUMENT
    IMG_AddInstrumentFunction(instrumentImage, 0);
#endif
    PIN_AddFiniFunction(fini, 0);
    init();
    PIN_StartProgram();
    return 0;
}
