#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: embed <output.cpp> <root> <files...>\n";
        return 1;
    }
    std::filesystem::path root = argv[2];
    std::ofstream out(argv[1], std::ios::binary);
    out << "#include \"assets.hpp\"\n\n";
    std::vector<std::pair<std::string, std::size_t>> entries;
    for (int i = 3; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string path = "/" + std::filesystem::relative(argv[i], root).generic_string();
        out << "static const unsigned char asset" << entries.size() << "[] = {";
        for (std::size_t j = 0; j < bytes.size(); ++j) {
            out << (j % 32 == 0 ? "\n" : "") << static_cast<int>(bytes[j]) << ",";
        }
        if (bytes.empty()) {
            out << "0";
        }
        out << "\n};\n";
        entries.emplace_back(path, bytes.size());
    }
    out << "\nextern const Asset kAssets[] = {\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        out << "    {\"" << entries[i].first << "\", asset" << i << ", " << entries[i].second << "},\n";
    }
    out << "};\n\nextern const std::size_t kAssetCount = " << entries.size() << ";\n";
    return out ? 0 : 1;
}
