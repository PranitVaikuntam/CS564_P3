#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <iostream>
#include <stdio.h>
#include "page.h"
#include "buf.h"

#define ASSERT(c)  { if (!(c)) { \
		       cerr << "At line " << __LINE__ << ":" << endl << "  "; \
                       cerr << "This condition should hold: " #c << endl; \
                       exit(1); \
		     } \
                   }

//----------------------------------------
// Constructor of the class BufMgr
//----------------------------------------

BufMgr::BufMgr(const int bufs)
{
    numBufs = bufs;

    bufTable = new BufDesc[bufs];
    memset(bufTable, 0, bufs * sizeof(BufDesc));
    for (int i = 0; i < bufs; i++) 
    {
        bufTable[i].frameNo = i;
        bufTable[i].valid = false;
    }

    bufPool = new Page[bufs];
    memset(bufPool, 0, bufs * sizeof(Page));

    int htsize = ((((int) (bufs * 1.2))*2)/2)+1;
    hashTable = new BufHashTbl (htsize);  // allocate the buffer hash table

    clockHand = bufs - 1;
}


BufMgr::~BufMgr() {

    // flush out all unwritten pages
    for (int i = 0; i < numBufs; i++) 
    {
        BufDesc* tmpbuf = &bufTable[i];
        if (tmpbuf->valid == true && tmpbuf->dirty == true) {

#ifdef DEBUGBUF
            cout << "flushing page " << tmpbuf->pageNo
                 << " from frame " << i << endl;
#endif

            tmpbuf->file->writePage(tmpbuf->pageNo, &(bufPool[i]));
        }
    }

    delete [] bufTable;
    delete [] bufPool;
}


const Status BufMgr::allocBuf(int & frame) 
{

    // keeps track of frames visited
    unsigned int numScanned = 0;
    
    // Uses clock algorithm to search buffer pool 2x for a free frame, or to decide which page to replace for a free frame
    while (numScanned < 2 * numBufs) {

        // advances the clock pointer
        advanceClock();
        
        // references the current frame the clock hand is pointering atm
        BufDesc* currFrame = &bufTable[clockHand];
        
        // ensures that frame has NO valid page (invalid)...
        if (!currFrame->valid) {
            frame = clockHand; //...then set frame to current clock pointer to be used
            return OK; 
        }
        
        // ensures that the frame is not pinned (no active users)....
        if (currFrame->pinCnt > 0) {
            numScanned++; // increments frames to indicated visited
            continue; // ...then skips to next frame
        }
        
        // ensures the refeneced bit is false (not recently accessed)...
        if (currFrame->refbit) {
            bufStats.accesses++; //increments BufStats accesses when frame is now accessed 
            currFrame->refbit = false; // sets reference bit from true to false = recently accessed
            numScanned++; // incremets frame visitation
            continue; // then skips to next frame
        }
        
        // ensures that the page copy is in sync in both buffer pool and in disk
        // So, if page is already in frame of bufferpool (dirty)....
        if (currFrame->dirty) {
            // ...then buffer manager frees the frame by writing page in frame to disk
            Status status = currFrame->file->writePage(currFrame->pageNo, &bufPool[clockHand]);
            // checks if write status was successfuk
            if (status != OK) {
                return UNIXERR; // ...if not, return UNIXERR error
            }
            bufStats.diskwrites++; // ...otherwise if diskwrite was OK, then incremets bufStats writes
        }
        
        // ..Finally when page not dirty, frame has valid page, and not pinned...
        Status status = hashTable->remove(currFrame->file, currFrame->pageNo); /// then removes page from hash table
        // ensures that removal of page was succeded
        if (status != OK) {
            return HASHTBLERROR; // ...otherwsie returns error
        }
        
        currFrame->Clear(); //...then frame is cleared 
        frame = clockHand; // and sets frame to current clock pointer
        return OK; // DONE
    }
        return BUFFEREXCEEDED; // otherwise returns error when all buffer frames are pinned
}

	
const Status BufMgr::readPage(File* file, const int PageNo, Page*& page)
{
    int frameNo = 0; 
    Status lookupStatus = hashTable->lookup(file, PageNo, frameNo); 

    // CASE 1: When page is not in the buffer pool
    if (lookupStatus == HASHNOTFOUND){

        //Allocates a buffer frame
        lookupStatus = allocBuf(frameNo); 
        if(lookupStatus!=OK){
            return lookupStatus; 
        }

        //Check if page is in buffer pool
        Status readStatus = file->readPage(PageNo, &(bufPool[frameNo]));
        if(readStatus != OK){
            return readStatus;
        }
        bufStats.diskreads++;


        //Insert page into hashtable
        Status insertStatus = hashTable->insert(file, PageNo, frameNo);
        if(insertStatus != OK){
            return insertStatus; 
        }

        //Edit the corresponding frame
        bufTable[frameNo].Set(file, PageNo);

        ////page points to the frame used and will be returned
        page = &(bufPool[frameNo]);
        return OK;

    // CASE 2: When page is in the buffer pool...
    } else if (lookupStatus == OK){ 
        //Edit the corresponding frame
        BufDesc &currFrame = bufTable[frameNo]; 
        currFrame.refbit = true;
        currFrame.pinCnt++;
        
        //page points to the frame used and will be returned
        page = &(bufPool[frameNo]);
        return OK; 
    }
    return lookupStatus; //Shouldn't reach here. Return lookup status
}


const Status BufMgr::unPinPage(File* file, const int PageNo, 
			       const bool dirty) 
{
    //Looks up up the page in the hash table
    //The page should be in the table
    int frameNo = 0;
    Status lookupStatus = hashTable->lookup(file, PageNo, frameNo);
    if(lookupStatus != OK){
        return lookupStatus; 
    }

    //Get the corresponding frame
    BufDesc &currFrame = bufTable[frameNo]; 

    //Decrement pin count if greater than 0
    if(currFrame.pinCnt <= 0){
        return PAGENOTPINNED; 
    }
    currFrame.pinCnt--;

    //Set dirty bit if necessary
    if(dirty){
        currFrame.dirty = true;
    }

    return OK; 
}

const Status BufMgr::allocPage(File* file, int& pageNo, Page*& page) 
{
    //Allocates a new page in the file
    Status allocStatus = file->allocatePage(pageNo);
    if(allocStatus != OK){
        return allocStatus; 
    }

    //Allocates a buffer frame
    int frameNo = 0;
    Status bufAllocStatus = allocBuf(frameNo);
    if(bufAllocStatus != OK){
        return bufAllocStatus; 
    }

    //Inserts the page into the hash table
    //PageNo is edited and will be returned
    Status insertStatus = hashTable->insert(file, pageNo, frameNo);
    if(insertStatus != OK){
        return insertStatus; 
    }

    //Edit the corresponding frame
    bufTable[frameNo].Set(file, pageNo);

    //page points to the frame used and will be returned
    page = &(bufPool[frameNo]);
    return OK;
}

const Status BufMgr::disposePage(File* file, const int pageNo) 
{
    // see if it is in the buffer pool
    Status status = OK;
    int frameNo = 0;
    status = hashTable->lookup(file, pageNo, frameNo);
    if (status == OK)
    {
        // clear the page
        bufTable[frameNo].Clear();
    }
    status = hashTable->remove(file, pageNo);

    // deallocate it in the file
    return file->disposePage(pageNo);
}

const Status BufMgr::flushFile(const File* file) 
{
  Status status;

  for (int i = 0; i < numBufs; i++) {
    BufDesc* tmpbuf = &(bufTable[i]);
    if (tmpbuf->valid == true && tmpbuf->file == file) {

      if (tmpbuf->pinCnt > 0)
	  return PAGEPINNED;

      if (tmpbuf->dirty == true) {
#ifdef DEBUGBUF
	cout << "flushing page " << tmpbuf->pageNo
             << " from frame " << i << endl;
#endif
	if ((status = tmpbuf->file->writePage(tmpbuf->pageNo,
					      &(bufPool[i]))) != OK)
	  return status;

	tmpbuf->dirty = false;
      }

      hashTable->remove(file,tmpbuf->pageNo);

      tmpbuf->file = NULL;
      tmpbuf->pageNo = -1;
      tmpbuf->valid = false;
    }

    else if (tmpbuf->valid == false && tmpbuf->file == file)
      return BADBUFFER;
  }
  
  return OK;
}


void BufMgr::printSelf(void) 
{
    BufDesc* tmpbuf;
  
    cout << endl << "Print buffer...\n";
    for (int i=0; i<numBufs; i++) {
        tmpbuf = &(bufTable[i]);
        cout << i << "\t" << (char*)(&bufPool[i]) 
             << "\tpinCnt: " << tmpbuf->pinCnt;
    
        if (tmpbuf->valid == true)
            cout << "\tvalid\n";
        cout << endl;
    };
}


