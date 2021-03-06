/****************************************************************************
 * Copyright (C) 2009, 2010, 2011, 2012 by Kapil Arya, Gene Cooperman,      *
 *                                     Tyler Denniston, and Ana-Maria Visan *
 * {kapil,gene,tyler,amvisan}@ccs.neu.edu                                   *
 *                                                                          *
 * This file is part of FReD.                                               *
 *                                                                          *
 * FReD is free software: you can redistribute it and/or modify             *
 * it under the terms of the GNU General Public License as published by     *
 * the Free Software Foundation, either version 3 of the License, or        *
 * (at your option) any later version.                                      *
 *                                                                          *
 * FReD is distributed in the hope that it will be useful,                  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 * GNU General Public License for more details.                             *
 *                                                                          *
 * You should have received a copy of the GNU General Public License        *
 * along with FReD.  If not, see <http://www.gnu.org/licenses/>.            *
 ****************************************************************************/

/*
 * TODO:
 *
 * 1. How is my_clone_id handled in case of exec()? How do we make sure that we
 *    still write to the same log assigned for this thread ?
 * 2. Get rid of all XXX_return events.
 *
 */

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "constants.h"
#include "log.h"
#include "synchronizationlogging.h"
#include "fred_wrappers.h"
#include "fred_interface.h"
#include "autogen/wrapper_util.h"
#include "util.h"
#include "jassert.h"

static log_entry_t _currentEntry = EMPTY_LOG_ENTRY;
LIB_PRIVATE int sync_logging_branch = SYNC_NOOP;

static void fred_interface_get_shm_file_name(char *name)
{
  dmtcp::ostringstream o;
  o << dmtcp_get_tmpdir() << "/fred-shm." << getpid();
  strcpy(name, o.str().c_str());
}

static size_t getEntrySize(log_entry_t *entry) {
  if (getNumActualFieldsInLogEvent(entry) > 0 || !entry->isRetvalZero()) {
    return log_event_header_size + getLogEventSize(entry);
  }
  return log_event_header_size;
}

void dmtcp::SynchronizationLog::initialize(const char *path, size_t size)
{
  bool mapWithNoReserveFlag = SYNC_IS_RECORD;

  JASSERT(_startAddr == NULL);
  JASSERT(_log == NULL);
  JASSERT(_index == 0);
  JASSERT(_size == NULL);
  JASSERT(_dataSize == NULL);
  JASSERT(_entryIndex == 0);
  JASSERT(_numEntries == NULL);
  JASSERT(_sharedInterfaceInfo == NULL);

  /* map_in calls init_common if appropriate. */
  map_in(path, size, mapWithNoReserveFlag);

  init_shm();

  JTRACE ("Initialized global synchronization log path to" )
    (_path) ((long)_startAddr) (*_size) (mapWithNoReserveFlag);

  if (_entryOffsetMarker > 0) {
    /* This means we checkpointed during record/replay. Restore the
       log to that point. */
    JASSERT(_entryIndexMarker > 0);
    log_entry_t temp_entry = EMPTY_LOG_ENTRY;
    int entrySize = getEntryAtOffset(temp_entry, _entryOffsetMarker);

    _index = _entryOffsetMarker;
    _entryIndex = _entryIndexMarker;
    _sharedInterfaceInfo->current_clone_id = temp_entry.cloneId();
    _sharedInterfaceInfo->current_log_entry_index = _entryIndex;

    JTRACE ("Restored log to index and offset from intermediate checkpoint.")
      ( _entryIndexMarker ) ( _entryOffsetMarker );

    // Reset for the next checkpoint.
    _entryOffsetMarker = 0;
    _entryIndexMarker = 0;
  }

  if (!isEndOfLog() && SYNC_IS_REPLAY) {
    // Load the new entry.
    getEntryAtOffset(_currentEntry, getIndex());
  }
}

/* Initialize shared memory region to be used by fred_command. */
void dmtcp::SynchronizationLog::init_shm()
{
  char name[PATH_MAX];
  int fd, retval;
  void *mmapAddr = (void*) _sharedInterfaceInfo;

  fred_interface_get_shm_file_name(name);

  JASSERT((void*)_sharedInterfaceInfo != NULL || SYNC_IS_RECORD);

  fd = open(name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  JASSERT ( fd != -1 ) (name) ( strerror(errno) );

  retval = ftruncate(fd, FRED_INTERFACE_SHM_SIZE);
  JASSERT ( retval != -1 ) ( strerror(errno) );
  JTRACE ( "Opened shared memory region." ) ( name );

  if (mmapAddr == NULL) {
    JASSERT(SYNC_IS_RECORD);
#ifdef FIREFOX_SUPPORT
    mmapAddr = (void*) 0x2f000000000L;
#endif
  }

  _sharedInterfaceInfo =
    (fred_interface_info_t *) _real_mmap(mmapAddr, FRED_INTERFACE_SHM_SIZE,
                                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  JASSERT((void*)_sharedInterfaceInfo != MAP_FAILED);
  JTRACE ( "Mapped shared memory region." ) ( _sharedInterfaceInfo );
  close(fd);

  _sharedInterfaceInfo->total_entries = *_numEntries;
  _sharedInterfaceInfo->total_threads = *_numThreads;
  _sharedInterfaceInfo->breakpoint_at_index = FRED_INTERFACE_NO_BP;

  LogMetadata *metadata = (LogMetadata *) _startAddr;

  if (SYNC_IS_RECORD) {
    JTRACE("RECORD; filling in recordedSharedInterfaceInfoMapAddr.")
      ((long)(void*)_sharedInterfaceInfo);

    JASSERT(_startAddr);
    LogMetadata *metadata = (LogMetadata *) _startAddr;
    metadata->recordedSharedInterfaceInfoMapAddr = _sharedInterfaceInfo;
  }
}

/* Destroy shared memory region. */
void dmtcp::SynchronizationLog::destroy_shm()
{
  if (_sharedInterfaceInfo == NULL) {
    return;
  }

  char name[PATH_MAX];
  fred_interface_get_shm_file_name(name);

  _real_munmap(_sharedInterfaceInfo, FRED_INTERFACE_SHM_SIZE);
  unlink(name);
}

void dmtcp::SynchronizationLog::init_common(size_t size)
{
  JASSERT(sizeof (LogMetadata) < LOG_OFFSET_FROM_START) (sizeof(LogMetadata));

  LogMetadata *metadata = (LogMetadata *) _startAddr;

  _numEntries = &(metadata->numEntries);
  _numThreads = &(metadata->numThreads);
  _dataSize = &(metadata->dataSize);
  _size = &(metadata->size);
  _recordedStartAddr = &(metadata->recordedStartAddr);

  _sharedInterfaceInfo =
    (fred_interface_info_t *) metadata->recordedSharedInterfaceInfoMapAddr;

  *_size = size;
  _log = _startAddr + LOG_OFFSET_FROM_START;

  if (*_recordedStartAddr == NULL) {
    JASSERT(SYNC_IS_RECORD);
    *_numThreads = 0;
    JASSERT(_startAddr != NULL && _startAddr != MAP_FAILED);
    JTRACE("RECORD; filling in _recordedStartAddr.") ((long)_startAddr);
    *_recordedStartAddr = _startAddr;
  }
}

void dmtcp::SynchronizationLog::destroy(int mode)
{
  /* When the log is destroyed, we save our place in the log so we can
     restore to that exact entry when we restore from the checkpoint
     image. For the first checkpoint, this will be 0 to indicate there
     are no entries. For subsequent checkpoints during replay, it will
     be the point in the log from which to start replay. */
  if (mode == SYNC_RECORD) {
    // Checkpoint during RECORD mode.
    _entryOffsetMarker = getDataSize();
    if (_numEntries != NULL) { // Will be NULL on first checkpoint.
      _entryIndexMarker  = __sync_fetch_and_add(_numEntries, 0);
    }
  } else {
    _entryOffsetMarker = getIndex();
    _entryIndexMarker  = _entryIndex;
  }

  if (_startAddr != NULL) {
    unmap();
  }
  _startAddr = _log = NULL;
  _path = "";
  _size = NULL;
  _index = 0;
  _entryIndex = 0;
  _dataSize = NULL;
  _numEntries = NULL;
  destroy_shm();
  _sharedInterfaceInfo = NULL;
}

void dmtcp::SynchronizationLog::unmap()
{
  if (_startAddr == NULL) {
    return;
  }
  // Save the size in case we want to remap after this unmap:
  _savedSize = *_size;
  JASSERT(_real_munmap(_startAddr, *_size) == 0) (JASSERT_ERRNO) (*_size) (_startAddr);
}

void dmtcp::SynchronizationLog::map_in(const char *path, size_t size,
                                       bool mapWithNoReserveFlag)
{
  int fd;
  int mmapProt = PROT_READ | PROT_WRITE;
  int mmapFlags = MAP_SHARED;
  void *mmapAddr = NULL, *tempAddr = NULL;
  LogMetadata *tempMetadata;

  JASSERT(path != NULL);
  fd = _real_open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  JASSERT(fd != -1);

  if (SYNC_IS_RECORD) {
    JASSERT(_real_lseek(fd, size, SEEK_SET) == (off_t)size);
    Util::writeAll(fd, "", 1);
  }

  if (mapWithNoReserveFlag) {
    // FIXME: Instead of MAP_NORESERVE, we may also choose to back it with
    // /dev/null which would also _not_ allocate pages until needed.
    mmapFlags |= MAP_NORESERVE;
  }

  tempAddr = _real_mmap(NULL, LOG_OFFSET_FROM_START, mmapProt,
                        mmapFlags, fd, 0);
  JASSERT(tempAddr != MAP_FAILED);
  tempMetadata = (LogMetadata *) tempAddr;
  mmapAddr = tempMetadata->recordedStartAddr;

#ifdef FIREFOX_SUPPORT
  if (mmapAddr == NULL) {
    JASSERT(SYNC_IS_RECORD);
    mmapAddr = (void*) 0x3f000000000L;
  }
#endif

  if (mmapAddr != NULL) {
    mmapFlags |= MAP_FIXED;
  }
  _real_munmap(tempAddr, LOG_OFFSET_FROM_START);

  _startAddr = (char*) _real_mmap(mmapAddr, size, mmapProt, mmapFlags, fd, 0);
  if (mmapAddr != NULL) {
    JASSERT ( (void *)_startAddr == mmapAddr );
  }

  _real_close(fd);
  _path = path == NULL ? "" : path;
  init_common(size);
}

void dmtcp::SynchronizationLog::map_in()
{
  char path_copy[RECORD_LOG_PATH_MAX] = {'\0'};
  strncpy(path_copy, _path.c_str(), RECORD_LOG_PATH_MAX);
  /* We don't want to pass a pointer to _path, because that could be reset
     as part of a subsequent call to destroy(). */
  map_in(path_copy, _savedSize, false);
}

int dmtcp::SynchronizationLog::advanceToNextEntry()
{
  if (_sharedInterfaceInfo->breakpoint_at_index == _entryIndex + 1) {
    // A breakpoint has been hit. The next thread should wait
    _sharedInterfaceInfo->breakpoint_at_index = FRED_INTERFACE_BP_HIT;
  }

  int entrySize = getEntrySize(&_currentEntry);
  JASSERT(entrySize > 0);
  atomicIncrementIndex(entrySize);
  atomicIncrementEntryIndex();

  if (isEndOfLog()) {
    return 0;
  }

  // Load the new entry.
  getEntryAtOffset(_currentEntry, getIndex());

  /* Keep interface info up to date. */
  _sharedInterfaceInfo->current_clone_id = _currentEntry.cloneId();
  _sharedInterfaceInfo->current_log_entry_index = _entryIndex;

  return 1;
}

void dmtcp::SynchronizationLog::checkForBreakpoint()
{
  // A breakpoint has been hit. wait here.
  if (_sharedInterfaceInfo->breakpoint_at_index == FRED_INTERFACE_BP_HIT) {
    // XXX: Can this be a condition variable wait instead?
    while (1) {
      // Wait until the breakpoint has been cleared.
      if (_sharedInterfaceInfo->breakpoint_at_index == FRED_INTERFACE_NO_BP) {
        break;
      }
      usleep(1);
    }
  }
}

log_entry_t& dmtcp::SynchronizationLog::getCurrentEntry()
{
  return _currentEntry;
}

// Reads the entry from log and returns the length of entry
int dmtcp::SynchronizationLog::getEntryAtOffset(log_entry_t& entry, size_t index)
{
  size_t currentDataSize = getDataSize();
  if (index == currentDataSize || getEntryHeaderAtOffset(entry, index) == 0) {
    entry = EMPTY_LOG_ENTRY;
    return 0;
  }

  JASSERT(entry.eventId() > 0);
  size_t event_size = getLogEventSize(&entry);

  if (getNumActualFieldsInLogEvent(&entry) > 0 || !entry.isRetvalZero()) {
    JASSERT(event_size > 0);
    if (index + log_event_header_size + event_size > currentDataSize) {
      JASSERT ((index + log_event_header_size + event_size) <= currentDataSize)
        (index) (log_event_header_size) (event_size) (currentDataSize);
    }
    memcpy(&entry.edata, &_log[index + log_event_header_size], event_size);
    return log_event_header_size + event_size;
  }
  return log_event_header_size;
}

size_t dmtcp::SynchronizationLog::appendEntry(log_entry_t& entry)
{
  log_off_t offset;

  size_t eventSize = getEntrySize(&entry);
  JASSERT( eventSize >= 0 );
  offset = atomicIncrementOffset(eventSize);
  __sync_fetch_and_add(_numEntries, 1);

  JASSERT(eventSize == writeEntryAtOffset(entry, offset));
  return offset;
}

static void validateEntry(const log_entry_t& entry, size_t offset)
{
  JASSERT(offset != INVALID_LOG_OFFSET);

  // only allow it for pthread_create and malloc calls
  JASSERT(entry.eventId() == pthread_create_event ||
          entry.eventId() == pthread_join_event ||
          entry.eventId() == pthread_rwlock_unlock_event ||
          entry.eventId() == pthread_mutex_unlock_event ||
          entry.eventId() == malloc_event ||
          entry.eventId() == libc_memalign_event ||
          entry.eventId() == calloc_event ||
          entry.eventId() == realloc_event);

#ifdef DEBUG
  log_entry_t old_entry = EMPTY_LOG_ENTRY;
  JASSERT(getEntryAtOffset(old_entry, offset) != 0);

  // Only allow replacing events of the same type. Allowing it for differing
  // types (which means differing sizes) would take more work.
  JASSERT(entry.eventId() == old_entry.eventId());

  if (entry.eventId() == pthread_create_event) {
    JASSERT(IS_EQUAL_FIELD(entry, old_entry, pthread_create, thread) &&
            IS_EQUAL_FIELD(entry, old_entry, pthread_create, thread) &&
            IS_EQUAL_FIELD(entry, old_entry, pthread_create, start_routine) &&
            IS_EQUAL_FIELD(entry, old_entry, pthread_create, attr) &&
            IS_EQUAL_FIELD(entry, old_entry, pthread_create, arg));
  } else if (entry.eventId() == malloc_event) {
    JASSERT(IS_EQUAL_FIELD(entry, old_entry, malloc, size));
  }
#endif
}

void dmtcp::SynchronizationLog::updateEntry(const log_entry_t& entry,
                                            size_t offset)
{
  JASSERT(offset <= *_dataSize);
  validateEntry(entry, offset);
  writeEntryAtOffset(entry, offset);
}

void dmtcp::SynchronizationLog::updateEntryHeader(const log_entry_t& entry,
                                                  size_t offset)
{
  JASSERT(offset <= *_dataSize);
  validateEntry(entry, offset);
  writeEntryHeaderAtOffset(entry, offset);
}

/* Move appropriate markers to the end, so that we enter "append" mode. */
void dmtcp::SynchronizationLog::moveMarkersToEnd()
{
  _index = getDataSize();
  _entryIndex = *_numEntries;
}

int dmtcp::SynchronizationLog::writeEntryAtOffset(const log_entry_t& entry,
                                                  size_t index)
{
  if (__builtin_expect(_startAddr == 0, 0)) {
    JASSERT(false);
  }

  int event_size = getLogEventSize(&entry);
  JASSERT( event_size > 0 );

  JASSERT ((LOG_OFFSET_FROM_START + index +
            log_event_header_size + event_size) < *_size)
    ( *_size ) .Text ("Log size too small. Please increase MAX_LOG_LENGTH"
                      " in synchronizationlogging.h");

  size_t ret = writeEntryHeaderAtOffset(entry, index);

  if (getNumActualFieldsInLogEvent(&entry) > 0 || !entry.isRetvalZero()) {
    JASSERT(event_size > 0);
    memcpy(&_log[index + ret], &entry.edata, event_size);
    ret += event_size;
  }
  return ret;
}

size_t dmtcp::SynchronizationLog::getEntryHeaderAtOffset(log_entry_t& entry,
                                                         size_t index)
{
  size_t currentDataSize = getDataSize();
  JASSERT ((index + log_event_header_size) <= currentDataSize)
    (index) (currentDataSize);

  memcpy(&entry.header, &_log[index], log_event_header_size);

  if (entry.cloneId() == 0) {
    return 0;
  }
  return log_event_header_size;
}

size_t dmtcp::SynchronizationLog::writeEntryHeaderAtOffset(const log_entry_t& entry,
                                                           size_t index)
{
  JASSERT(entry.cloneId() > 0);

  memcpy(&_log[index], &entry.header, log_event_header_size);
  return log_event_header_size;
}

size_t dmtcp::SynchronizationLog::getIndex()
{
  return __sync_fetch_and_add(&_index, 0);
}

size_t dmtcp::SynchronizationLog::atomicIncrementIndex(log_off_t delta)
{
  return __sync_fetch_and_add(&_index, delta);
}

size_t dmtcp::SynchronizationLog::atomicIncrementEntryIndex()
{
  return __sync_fetch_and_add(&_entryIndex, 1);
}

/*
 * This function should be used instead of dereferencing _dataSize directly.
 * It adds error checking for NULL, and correct atomic functionality.
 */
size_t dmtcp::SynchronizationLog::getDataSize()
{
  if (_dataSize == NULL) {
    return 0;
  } else {
    return __sync_fetch_and_add(_dataSize, 0);
  }
}

/*
 * This function should be used instead of setting the value of
 * _dataSize directly.  It adds error checking for NULL, and correct
 * atomic functionality.
 */
void dmtcp::SynchronizationLog::setDataSize(log_off_t newVal)
{
  JASSERT(_dataSize != NULL);
  __sync_val_compare_and_swap(_dataSize, *_dataSize, newVal);
}

void dmtcp::SynchronizationLog::incrementNumberThreads()
{
  if (_numThreads != NULL) {
    __sync_fetch_and_add(_numThreads, 1);
  }
}

inline log_off_t
dmtcp::SynchronizationLog::atomicIncrementOffset(log_off_t delta)
{
  JASSERT(_dataSize != NULL);
  return __sync_fetch_and_add(_dataSize, delta);
}
