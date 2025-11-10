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
    // Implementation of the clock algorithm to find a free frame
    // We need to scan the buffer pool to find a frame to allocate
    
    unsigned int numScanned = 0;
    
    // Keep scanning until we've gone around the clock twice
    // This ensures we give every frame a chance even if refbits are set
    while (numScanned < 2 * numBufs) {
        // Advance the clock hand
        advanceClock();
        
        BufDesc* currFrame = &bufTable[clockHand];
        
        // Case 1: Frame is not valid (empty/unused frame)
        if (!currFrame->valid) {
            // Found an empty frame - we can use it immediately
            frame = clockHand;
            return OK;
        }
        
        // Case 2: Frame is valid, check if it's pinned
        if (currFrame->pinCnt > 0) {
            // Page is pinned, can't evict it, skip to next frame
            numScanned++;
            continue;
        }
        
        // Case 3: Frame is valid and not pinned, check refbit
        if (currFrame->refbit) {
            // Recently referenced - give it another chance
            // Clear the refbit and move on
            currFrame->refbit = false;
            numScanned++;
            bufStats.accesses++;
            continue;
        }
        
        // Case 4: Found a victim frame!
        // refbit is false (not recently referenced) and not pinned
        
        // If the page is dirty, write it back to disk before evicting
        if (currFrame->dirty) {
            Status status = currFrame->file->writePage(currFrame->pageNo, &bufPool[clockHand]);
            if (status != OK) {
                return UNIXERR;
            }
            bufStats.diskwrites++;
        }
        
        // Remove the old page from the hash table
        Status status = hashTable->remove(currFrame->file, currFrame->pageNo);
        if (status != OK) {
            return HASHTBLERROR;
        }
        
        // Clear the frame descriptor for the new page
        currFrame->Clear();
        
        // Return the frame number
        frame = clockHand;
        return OK;
    }
    
    // If we've scanned twice around and all frames are pinned
    return BUFFEREXCEEDED;
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


