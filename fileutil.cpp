#include "fileutil.h"

#include "panic.h"

#include <fstream>

std::vector<char> load_file(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
        panic("failed to open %s\n", std::string(path).c_str());

    auto *buf = file.rdbuf();

    const std::size_t size = buf->pubseekoff(0, file.end, file.in);
    buf->pubseekpos(0, file.in);

    std::vector<char> data(size + 1);
    buf->sgetn(data.data(), size);
    data[size] = 0;

    file.close();

    return data;
}
