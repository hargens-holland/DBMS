#include "heapfile.h"
#include "error.h"
// Holland Hargens, student id = 9084887216
// Joshua Hall,     student id = 9083668948
// Nicholas fleming student id = 9085248046
/*this file purpose is to provide the core logic for creating and managing
heap files in Minirel. It includes functions for initializing a new heap file,
opening and reading its header page, scanning through its records with
optional filtering, and inserting new records (creating pages on demand).
The classes here (HeapFile, HeapFileScan, InsertFileScan) maintain the
linked structure of data pages and handle pinning/unpinning pages as
needed, ensuring that record reads and writes are buffered efficiently.*/

// routine to create a heapfile
const Status createHeapFile(const string fileName)
{
    File *file;
    Status status;
    FileHdrPage *hdrPage;
    int hdrPageNo;
    int newPageNo;
    Page *newPage;

    // try to open the file. This should return an error
    status = db.openFile(fileName, file);
    if (status != OK)
    {
        // file doesn't exist. First create it and allocate
        // an empty header page and data page.
        db.createFile(fileName);
        // now open it to initialize it, or you will get a segFault somewhere
        status = db.openFile(fileName, file);
        if (status != OK)
            return status;

        // Now, allocate the header page
        status = bufMgr->allocPage(file, hdrPageNo, newPage);
        if (status != OK)
        {
            db.closeFile(file);
            return status;
        }

        // Cast the page to a FileHdrPage and initialize it
        hdrPage = (FileHdrPage *)newPage;
        strncpy(hdrPage->fileName, fileName.c_str(), MAXNAMESIZE);
        hdrPage->recCnt = 0;
        hdrPage->pageCnt = 1; // Will be incremented when we add the first data page

        // Allocate the first data page
        status = bufMgr->allocPage(file, newPageNo, newPage);
        if (status != OK)
        {
            bufMgr->unPinPage(file, hdrPageNo, false);
            db.closeFile(file);
            return status;
        }

        // Initialize the data page
        newPage->init(newPageNo);

        // Update the header page with the first data page information
        hdrPage->firstPage = newPageNo;
        hdrPage->lastPage = newPageNo;
        hdrPage->pageCnt++;

        // Unpin both pages, marking them as dirty
        status = bufMgr->unPinPage(file, newPageNo, true);
        if (status != OK)
        {
            bufMgr->unPinPage(file, hdrPageNo, true);
            db.closeFile(file);
            return status;
        }

        status = bufMgr->unPinPage(file, hdrPageNo, true);
        if (status != OK)
        {
            db.closeFile(file);
            return status;
        }

        // Close the file
        status = db.closeFile(file);
        return status;
    }

    // File already exists
    db.closeFile(file);
    return FILEEXISTS;
}

// routine to destroy a heapfile
const Status destroyHeapFile(const string fileName)
{
    return (db.destroyFile(fileName));
}

// constructor opens the underlying file
HeapFile::HeapFile(const string &fileName, Status &returnStatus)
{
    Status status;
    Page *pagePtr;

    cout << "opening file " << fileName << endl;

    // open the file and read in the header page and the first data page
    if ((status = db.openFile(fileName, filePtr)) == OK)
    {
        // Read the header page
        status = filePtr->getFirstPage(headerPageNo);
        if (status != OK)
        {
            returnStatus = status;
            return;
        }

        status = bufMgr->readPage(filePtr, headerPageNo, pagePtr);
        if (status != OK)
        {
            returnStatus = status;
            return;
        }

        headerPage = (FileHdrPage *)pagePtr;
        hdrDirtyFlag = false;

        // Read the first data page
        if (headerPage->firstPage != -1)
        {
            curPageNo = headerPage->firstPage;
            status = bufMgr->readPage(filePtr, curPageNo, curPage);
            if (status != OK)
            {
                bufMgr->unPinPage(filePtr, headerPageNo, hdrDirtyFlag);
                returnStatus = status;
                return;
            }
            curDirtyFlag = false;
        }
        else
        {
            // No data pages yet
            curPage = NULL;
            curPageNo = -1;
            curDirtyFlag = false;
        }

        // Initialize curRec to NULLRID
        curRec.pageNo = -1;
        curRec.slotNo = -1;

        returnStatus = OK;
    }
    else
    {
        cerr << "open of heap file failed\n";
        returnStatus = status;
        return;
    }
}

// the destructor closes the file
HeapFile::~HeapFile()
{
    Status status;
    cout << "invoking heapfile destructor on file " << headerPage->fileName << endl;

    // see if there is a pinned data page. If so, unpin it
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
        curPage = NULL;
        curPageNo = -1;
        curDirtyFlag = false;
        if (status != OK)
            cerr << "error in unpin of date page\n";
    }

    // unpin the header page
    status = bufMgr->unPinPage(filePtr, headerPageNo, hdrDirtyFlag);
    if (status != OK)
        cerr << "error in unpin of header page\n";

    // Make sure all pages of the file are flushed to disk
    status = bufMgr->flushFile(filePtr);
    if (status != OK)
        cerr << "error in flushFile call\n";

    // Close the file
    status = db.closeFile(filePtr);
    if (status != OK)
    {
        cerr << "error in closefile call\n";
        Error e;
        e.print(status);
    }
}

// Return number of records in heap file

const int HeapFile::getRecCnt() const
{
    return headerPage->recCnt;
}

// retrieve an arbitrary record from a file.
// if record is not on the currently pinned page, the current page
// is unpinned and the required page is read into the buffer pool
// and pinned.  returns a pointer to the record via the rec parameter

const Status HeapFile::getRecord(const RID &rid, Record &rec)
{
    Status status;

    // cout<< "getRecord. record (" << rid.pageNo << "." << rid.slotNo << ")" << endl;

    // Check if requested record is on the current page
    if (curPage == NULL || rid.pageNo != curPageNo)
    {
        // Need to unpin current page if one is pinned
        if (curPage != NULL)
        {
            status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
            if (status != OK)
                return status;
            curPage = NULL;
        }

        // Read the page containing the requested record
        curPageNo = rid.pageNo;
        status = bufMgr->readPage(filePtr, curPageNo, curPage);
        if (status != OK)
            return status;
        curDirtyFlag = false;
    }

    // Get the record from the page
    status = curPage->getRecord(rid, rec);
    if (status == OK)
    {
        // Update curRec if successful
        curRec = rid;
    }

    return status;
}

HeapFileScan::HeapFileScan(const string &name,
                           Status &status) : HeapFile(name, status)
{
    filter = NULL;
}

const Status HeapFileScan::startScan(const int offset_,
                                     const int length_,
                                     const Datatype type_,
                                     const char *filter_,
                                     const Operator op_)
{
    if (!filter_)
    { // no filtering requested
        filter = NULL;
        return OK;
    }

    if ((offset_ < 0 || length_ < 1) ||
        (type_ != STRING && type_ != INTEGER && type_ != FLOAT) ||
        (type_ == INTEGER && length_ != sizeof(int) || type_ == FLOAT && length_ != sizeof(float)) ||
        (op_ != LT && op_ != LTE && op_ != EQ && op_ != GTE && op_ != GT && op_ != NE))
    {
        return BADSCANPARM;
    }

    offset = offset_;
    length = length_;
    type = type_;
    filter = filter_;
    op = op_;

    return OK;
}

const Status HeapFileScan::endScan()
{
    Status status;
    // generally must unpin last page of the scan
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
        curPage = NULL;
        curPageNo = 0;
        curDirtyFlag = false;
        return status;
    }
    return OK;
}

HeapFileScan::~HeapFileScan()
{
    endScan();
}

const Status HeapFileScan::markScan()
{
    // make a snapshot of the state of the scan
    markedPageNo = curPageNo;
    markedRec = curRec;
    return OK;
}

const Status HeapFileScan::resetScan()
{
    Status status;
    if (markedPageNo != curPageNo)
    {
        if (curPage != NULL)
        {
            status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
            if (status != OK)
                return status;
        }
        // restore curPageNo and curRec values
        curPageNo = markedPageNo;
        curRec = markedRec;
        // then read the page
        status = bufMgr->readPage(filePtr, curPageNo, curPage);
        if (status != OK)
            return status;
        curDirtyFlag = false; // it will be clean
    }
    else
        curRec = markedRec;
    return OK;
}

const Status HeapFileScan::scanNext(RID &outRid)
{
    Status status = OK;
    Record rec;

    while (true)
    {
        if (curPage == nullptr)
        {
            curPageNo = headerPage->firstPage;
            if (curPageNo == -1)
                return FILEEOF;

            status = bufMgr->readPage(filePtr, curPageNo, curPage);
            if (status != OK)
                return status;

            curDirtyFlag = false;
            status = curPage->firstRecord(curRec);
        }
        else
        {
            RID nextRid;
            status = curPage->nextRecord(curRec, nextRid);
            if (status == OK)
            {
                curRec = nextRid;
            }
            else
            {
                // Move to next page
                int nextPageNo;
                status = curPage->getNextPage(nextPageNo);

                bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
                curPage = nullptr;
                curPageNo = -1;
                curDirtyFlag = false;

                if (status != OK || nextPageNo == -1)
                    return FILEEOF;

                curPageNo = nextPageNo;
                status = bufMgr->readPage(filePtr, curPageNo, curPage);
                if (status != OK)
                    return status;

                curDirtyFlag = false;
                status = curPage->firstRecord(curRec);
            }
        }

        if (status != OK)
            continue;

        status = curPage->getRecord(curRec, rec);
        if (status != OK)
            return status;

        if (matchRec(rec))
        {
            outRid = curRec;
            return OK;
        }
    }
}

// returns pointer to the current record.  page is left pinned
// and the scan logic is required to unpin the page

const Status HeapFileScan::getRecord(Record &rec)
{
    return curPage->getRecord(curRec, rec);
}

// delete record from file.
const Status HeapFileScan::deleteRecord()
{
    Status status;

    // delete the "current" record from the page
    status = curPage->deleteRecord(curRec);
    curDirtyFlag = true;

    // reduce count of number of records in the file
    headerPage->recCnt--;
    hdrDirtyFlag = true;
    return status;
}

// mark current page of scan dirty
const Status HeapFileScan::markDirty()
{
    curDirtyFlag = true;
    return OK;
}

const bool HeapFileScan::matchRec(const Record &rec) const
{
    // no filtering requested
    if (!filter)
        return true;

    // see if offset + length is beyond end of record
    // maybe this should be an error???
    if ((offset + length - 1) >= rec.length)
        return false;

    float diff = 0; // < 0 if attr < fltr
    switch (type)
    {

    case INTEGER:
        int iattr, ifltr; // word-alignment problem possible
        memcpy(&iattr,
               (char *)rec.data + offset,
               length);
        memcpy(&ifltr,
               filter,
               length);
        diff = iattr - ifltr;
        break;

    case FLOAT:
        float fattr, ffltr; // word-alignment problem possible
        memcpy(&fattr,
               (char *)rec.data + offset,
               length);
        memcpy(&ffltr,
               filter,
               length);
        diff = fattr - ffltr;
        break;

    case STRING:
        diff = strncmp((char *)rec.data + offset,
                       filter,
                       length);
        break;
    }

    switch (op)
    {
    case LT:
        if (diff < 0.0)
            return true;
        break;
    case LTE:
        if (diff <= 0.0)
            return true;
        break;
    case EQ:
        if (diff == 0.0)
            return true;
        break;
    case GTE:
        if (diff >= 0.0)
            return true;
        break;
    case GT:
        if (diff > 0.0)
            return true;
        break;
    case NE:
        if (diff != 0.0)
            return true;
        break;
    }

    return false;
}

InsertFileScan::InsertFileScan(const string &name,
                               Status &status) : HeapFile(name, status)
{
    // Do nothing. Heapfile constructor will bread the header page and the first
    //  data page of the file into the buffer pool
}

InsertFileScan::~InsertFileScan()
{
    Status status;
    // unpin last page of the scan
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, true);
        curPage = NULL;
        curPageNo = 0;
        if (status != OK)
            cerr << "error in unpin of data page\n";
    }
}

// Insert a record into the file
const Status InsertFileScan::insertRecord(const Record &rec, RID &outRid)
{
    Page *newPage;
    int newPageNo;
    Status status;
    RID rid;

    if ((unsigned int)rec.length > PAGESIZE - DPFIXED)
        return INVALIDRECLEN;

    // If curPage is NULL, load the last page
    if (curPage == nullptr)
    {
        curPageNo = headerPage->lastPage;
        status = bufMgr->readPage(filePtr, curPageNo, curPage);
        if (status != OK)
            return status;
        curDirtyFlag = false;
    }

    // Try to insert on the current page
    status = curPage->insertRecord(rec, rid);
    if (status == OK)
    {
        outRid = rid;
        curRec = rid;
        curDirtyFlag = true;
        headerPage->recCnt++;
        hdrDirtyFlag = true;

        // Unpin the page
        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
        curPage = nullptr;
        curPageNo = -1;
        curDirtyFlag = false;

        return status;
    }

    // If page is full, allocate a new one
    if (status == NOSPACE)
    {
        status = bufMgr->allocPage(filePtr, newPageNo, newPage);
        if (status != OK)
            return status;

        newPage->init(newPageNo);
        curPage->setNextPage(newPageNo);
        curDirtyFlag = true;
        headerPage->lastPage = newPageNo;
        headerPage->pageCnt++;
        hdrDirtyFlag = true;

        // Unpin current full page
        bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);

        // Insert into new page
        curPage = newPage;
        curPageNo = newPageNo;
        curDirtyFlag = false;

        status = curPage->insertRecord(rec, rid);
        if (status != OK)
            return status;

        outRid = rid;
        curRec = rid;
        curDirtyFlag = true;
        headerPage->recCnt++;
        hdrDirtyFlag = true;

        // Unpin the new page
        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
        curPage = nullptr;
        curPageNo = -1;
        curDirtyFlag = false;

        return status;
    }

    return status;
}
