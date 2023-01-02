#ifndef _FILE_ENTRY_H_
#define _FILE_ENTRY_H_

#include <fatfs/ff.h>

class FileEntry {
   public:
    FileEntry() = default;

    FILINFO* GetFilinfo();

    const char* GetName() const;
    bool IsDirectory() const;
    unsigned int GetSize() const;
    unsigned int GetModifiedTS() const;
    unsigned int GetAttributes() const;

   private:
    bool IsFilenameValid() const;

   private:
    FILINFO filinfo;
};

#endif  // _FILE_ENTRY_H_
