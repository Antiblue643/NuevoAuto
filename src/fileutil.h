// fileutil.h

#ifndef NUEVOAUTO_FILEUTIL_H
#define NUEVOAUTO_FILEUTIL_H
#include <string>

class FileUtil {
public:
    std::string fname;
    FileUtil(const std::string& filename);
    bool writeStringToFile(const std::string& str);
};


#endif //NUEVOAUTO_FILEUTIL_H
