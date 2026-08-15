// fileutil.cpp

#include "fileutil.h"
#include <iostream>
#include <fstream>

FileUtil::FileUtil(const std::string& filename) {
    fname = filename;
}

bool FileUtil::writeStringToFile(const std::string &str) {
    // Open in append mode so multiple calls accumulate into the same file
    std::ofstream outFile(fname, std::ios::binary | std::ios::app);
    if (!outFile) {
        std::cerr << "Error opening file for writing: " << fname << std::endl;
        return false;
    }

    outFile.write(str.c_str(), str.size());
    if (!outFile) {
        std::cerr << "Error writing to file: " << fname << std::endl;
        return false;
    }
    outFile.close();
    return true;
}