#include "src/loaders/mesh_assimp.h"
#include "src/loaders/image_sniff.h"
#include "src/loaders/mesh.h"
#include "src/loaders/mesh_loader.h"
#include "src/math/light.h"
#include "src/math/linalg.h"
#include "src/render/texture.h"

#include <assimp/AssertHandler.h>
#include <assimp/Importer.hpp>
#include <assimp/color4.h>
#include <assimp/config.h>
#include <assimp/defs.h>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/texture.h>
#include <assimp/types.h>
#include <assimp/vector3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <istream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace
{
    bool finite(float v)
    {
        return std::isfinite(v);
    }

    bool finite(const aiVector3D &v)
    {
        return finite(v.x) && finite(v.y) && finite(v.z);
    }

    bool finite(const aiColor4D &v)
    {
        return finite(v.r) && finite(v.g) && finite(v.b) && finite(v.a);
    }

    vec3 to_vec3(const aiColor3D &v)
    {
        return { v.r, v.g, v.b };
    }

    vec3 to_vec3(const aiColor4D &v)
    {
        return { v.r, v.g, v.b };
    }

    float unit(float v)
    {
        return clamp(v, 0.0f, 1.0f);
    }

    // Some importers assert on malformed input immediately before throwing. Ignore those
    // assertions so debug and release builds reject the same files through the normal path.
    void note_assert_violation(const char *expression, const char *file, int line)
    {
        std::fprintf(stderr, "note: Assimp assertion ignored: %s (%s:%d)\n", expression, file, line);
    }

    // setAiAssertHandler writes a plain pointer. Install once to avoid concurrent stores.
    void install_assert_handler()
    {
        static const bool installed = []
        {
            Assimp::setAiAssertHandler(&note_assert_violation);
            return true;
        }();
        (void)installed;
    }

    std::string normalized_path(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }

    std::string lower_extension(const std::string &path)
    {
        const size_t pos = path.find_last_of('.');
        if (pos == std::string::npos || pos + 1 == path.size())
        {
            return {};
        }
        std::string out = path.substr(pos);
        std::transform(
            out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
        );
        return out;
    }

    uintmax_t file_bytes(const std::string &path)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        return ec ? 0 : size;
    }

    // TerragenLoader checks x * y * 2 in 32 bits. Walk its chunk layout and reject grids
    // whose declared height data exceeds the file before that multiplication can wrap.
    bool terragen_grid_fits_file(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return false;
        }
        char magic[16];
        if (!in.read(magic, sizeof magic))
        {
            return false;
        }
        // Leave unrelated malformed files to the importer.
        if (std::memcmp(magic, "TERRAGEN", 8) != 0 || std::memcmp(magic + 8, "TERRAIN ", 8) != 0)
        {
            return true;
        }
        const uintmax_t size = file_bytes(path);
        uint32_t grid_x = 0;
        uint32_t grid_y = 0;
        char tag[4];
        while (in.read(tag, sizeof tag))
        {
            if (std::memcmp(tag, "EOF ", 4) == 0)
            {
                return true;
            }
            if (std::memcmp(tag, "XPTS", 4) == 0 || std::memcmp(tag, "YPTS", 4) == 0 ||
                std::memcmp(tag, "SIZE", 4) == 0)
            {
                char value[2];
                if (!in.read(value, sizeof value))
                {
                    return false;
                }
                const auto raw = static_cast<uint16_t>(
                    (static_cast<unsigned>(static_cast<unsigned char>(value[0]))) |
                    ((static_cast<unsigned>(static_cast<unsigned char>(value[1]))) << 8u)
                );
                if (tag[0] == 'X')
                {
                    grid_x = raw;
                }
                else if (tag[0] == 'Y')
                {
                    grid_y = raw;
                }
                else
                {
                    grid_x = static_cast<uint32_t>(raw) + 1u;
                    grid_y = static_cast<uint32_t>(raw) + 1u;
                }
            }
            else if (std::memcmp(tag, "SCAL", 4) == 0)
            {
                if (!in.seekg(12, std::ios::cur))
                {
                    return false;
                }
            }
            else if (std::memcmp(tag, "CRAD", 4) == 0)
            {
                if (!in.seekg(4, std::ios::cur))
                {
                    return false;
                }
            }
            else if (std::memcmp(tag, "CRVM", 4) == 0)
            {
                if (!in.seekg(1, std::ios::cur))
                {
                    return false;
                }
            }
            else if (std::memcmp(tag, "ALTW", 4) == 0)
            {
                if (!in.seekg(4, std::ios::cur))
                {
                    return false;
                }
                if (grid_x >= 2 && grid_y >= 2)
                {
                    const std::streamoff pos = in.tellg();
                    if (pos < 0 || static_cast<uintmax_t>(pos) > size)
                    {
                        return false;
                    }
                    const auto remaining = size - static_cast<uintmax_t>(pos);
                    const uint64_t required = static_cast<uint64_t>(grid_x) * grid_y * 2u;
                    if (remaining < required)
                    {
                        return false;
                    }
                }
            }
            // Chunk bodies end on four-byte boundaries.
            const std::streamoff pos = in.tellg();
            const auto aligned = ((static_cast<uintmax_t>(pos) + 3u) & ~static_cast<uintmax_t>(3));
            if (pos < 0 || !in.seekg(static_cast<std::streamoff>(aligned), std::ios::beg))
            {
                return false;
            }
        }
        return true;
    }

    // Unreal's loader checks vertex indices against numTris instead of numVert. Validate
    // the fixed-size _d.3d records before it can rewire or overrun them.
    bool unreal_triangle_indices_in_bounds(const std::string &path)
    {
        std::string base;
        if (lower_extension(path) == ".3d")
        {
            const size_t underscore = path.find_last_of('_');
            if (underscore == std::string::npos)
            {
                return true;
            }
            base = path.substr(0, underscore);
        }
        else
        {
            const size_t dot_pos = path.find_last_of('.');
            if (dot_pos == std::string::npos)
            {
                return true;
            }
            base = path.substr(0, dot_pos);
        }
        std::ifstream in(base + "_d.3d", std::ios::binary);
        if (!in)
        {
            return true;
        }
        char header[48];
        if (!in.read(header, sizeof header))
        {
            return true;
        }
        const auto le16 = [](const char *bytes)
        {
            return static_cast<unsigned>(static_cast<unsigned char>(bytes[0])) |
                   (static_cast<unsigned>(static_cast<unsigned char>(bytes[1])) << 8u);
        };
        const unsigned num_tris = le16(header);
        const unsigned num_vert = le16(header + 2);
        for (unsigned i = 0; i < num_tris; i++)
        {
            char record[6];
            in.seekg(48 + (static_cast<long>(i) * 16), std::ios::beg);
            if (!in.read(record, sizeof record))
            {
                return true;
            }
            if (le16(record) >= num_vert || le16(record + 2) >= num_vert || le16(record + 4) >= num_vert)
            {
                return false;
            }
        }
        return true;
    }

    bool next_ogex_token(std::istream &in, std::string &token)
    {
        token.clear();
        char c = 0;
        while (in.get(c))
        {
            if (std::isspace(static_cast<unsigned char>(c)) != 0)
            {
                continue;
            }
            if (c == '/')
            {
                const int next = in.peek();
                if (next == '/')
                {
                    in.get();
                    while (in.get(c) && c != '\n' && c != '\r')
                    {
                    }
                    continue;
                }
                if (next == '*')
                {
                    in.get();
                    char previous = 0;
                    while (in.get(c) && (previous != '*' || c != '/'))
                    {
                        previous = c;
                    }
                    continue;
                }
            }
            if (c == '"')
            {
                token.push_back(c);
                bool escaped = false;
                while (in.get(c))
                {
                    if (token.size() <= 64)
                    {
                        token.push_back(c);
                    }
                    if (!escaped && c == '"')
                    {
                        return true;
                    }
                    escaped = !escaped && c == '\\';
                    if (c != '\\')
                    {
                        escaped = false;
                    }
                }
                return false;
            }
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_')
            {
                token.push_back(c);
                while (in.peek() != std::char_traits<char>::eof())
                {
                    const auto next = static_cast<unsigned char>(in.peek());
                    if (std::isalnum(next) == 0 && next != '_')
                    {
                        break;
                    }
                    if (token.size() <= 64)
                    {
                        token.push_back(static_cast<char>(in.get()));
                    }
                    else
                    {
                        in.get();
                    }
                }
                return true;
            }
            token.push_back(c);
            return true;
        }
        return false;
    }

    // Assimp parses but drops OpenGEX's up metric.
    char ogex_declared_up_axis(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return 0;
        }
        bool metric = false;
        bool up_metric = false;
        unsigned braces = 0;
        std::string token;
        while (next_ogex_token(in, token))
        {
            if (!metric)
            {
                metric = token == "Metric";
                continue;
            }
            if (braces == 0 && token == "\"up\"")
            {
                up_metric = true;
            }
            else if (token == "{")
            {
                braces++;
            }
            else if (token == "}" && braces > 0)
            {
                if (--braces == 0)
                {
                    metric = false;
                    up_metric = false;
                }
            }
            else if (up_metric && braces > 0 && token.size() == 3 && token.front() == '"' && token.back() == '"')
            {
                return token[1];
            }
        }
        return 0;
    }

    // Bound OFF's unchecked allocations by the minimum bytes each declared item needs.
    bool off_declared_counts_fit_file(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return true;
        }
        auto next_token = [&](std::string &token)
        {
            token.clear();
            for (;;)
            {
                const int next = in.peek();
                if (next == std::char_traits<char>::eof())
                {
                    return false;
                }
                if (std::isspace(static_cast<unsigned char>(next)) != 0)
                {
                    in.get();
                    continue;
                }
                if (next != '#')
                {
                    break;
                }
                char c = 0;
                while (in.get(c) && c != '\n' && c != '\r')
                {
                }
            }
            while (token.size() <= 64)
            {
                const int next = in.peek();
                if (next == std::char_traits<char>::eof() || std::isspace(static_cast<unsigned char>(next)) != 0 ||
                    next == '#')
                {
                    return !token.empty();
                }
                token.push_back(static_cast<char>(in.get()));
            }
            return false;
        };

        std::string token;
        if (!next_token(token))
        {
            return false;
        }
        const bool has_header = token.find_first_not_of("0123456789") != std::string::npos;
        const bool has_dimension = token.size() >= 4 && token.compare(token.size() - 4, 4, "nOFF") == 0;
        if (has_header && !next_token(token))
        {
            return false;
        }
        if (has_dimension && !next_token(token))
        {
            return false;
        }
        uint64_t counts[3]{};
        for (size_t i = 0; i < 3; i++)
        {
            if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos)
            {
                return false;
            }
            errno = 0;
            counts[i] = std::strtoull(token.c_str(), nullptr, 10);
            if (errno == ERANGE)
            {
                return false;
            }
            if (i + 1 < 3 && !next_token(token))
            {
                return false;
            }
        }
        const uintmax_t size = file_bytes(path);
        if (counts[0] > size / 6u)
        {
            return false;
        }
        const uintmax_t vertex_bytes = counts[0] * 6u;
        return counts[1] <= (size - vertex_bytes) / 2u;
    }

    bool skip_through(std::istream &in, std::string_view terminator)
    {
        size_t matched = 0;
        char c = 0;
        while (in.get(c))
        {
            if (c == terminator[matched])
            {
                if (++matched == terminator.size())
                {
                    return true;
                }
            }
            else
            {
                matched = c == terminator.front() ? 1 : 0;
            }
        }
        return false;
    }

    bool skip_xml_declaration(std::istream &in)
    {
        unsigned brackets = 0;
        char quote = 0;
        char c = 0;
        while (in.get(c))
        {
            if (quote != 0)
            {
                if (c == quote)
                {
                    quote = 0;
                }
                continue;
            }
            if (c == '"' || c == '\'')
            {
                quote = c;
            }
            else if (c == '[')
            {
                brackets++;
            }
            else if (c == ']' && brackets > 0)
            {
                brackets--;
            }
            else if (c == '>' && brackets == 0)
            {
                return true;
            }
        }
        return false;
    }

    // Return one for a texture tag, zero at EOF, and minus one for malformed markup.
    int next_xml_texture_tag(std::istream &in, std::string &tag)
    {
        char c = 0;
        while (in.get(c))
        {
            if (c != '<')
            {
                continue;
            }
            char first = 0;
            if (!in.get(first))
            {
                return -1;
            }
            if (first == '!')
            {
                char marker = 0;
                if (!in.get(marker))
                {
                    return -1;
                }
                if (marker == '-')
                {
                    char second = 0;
                    if (!in.get(second) || second != '-' || !skip_through(in, "-->"))
                    {
                        return -1;
                    }
                }
                else if (marker == '[')
                {
                    std::array<char, 6> suffix{};
                    if (!in.read(suffix.data(), static_cast<std::streamsize>(suffix.size())) ||
                        std::string_view(suffix.data(), suffix.size()) != "CDATA[" || !skip_through(in, "]]>"))
                    {
                        return -1;
                    }
                }
                else if (!skip_xml_declaration(in))
                {
                    return -1;
                }
                continue;
            }
            if (first == '?')
            {
                if (!skip_through(in, "?>"))
                {
                    return -1;
                }
                continue;
            }

            tag.clear();
            tag.push_back('<');
            tag.push_back(first);
            char quote = 0;
            while (in.get(c))
            {
                tag.push_back(c);
                if (quote != 0)
                {
                    if (c == quote)
                    {
                        quote = 0;
                    }
                }
                else if (c == '"' || c == '\'')
                {
                    quote = c;
                }
                else if (c == '>')
                {
                    break;
                }
            }
            if (!in && c != '>')
            {
                return -1;
            }
            size_t name_end = 1;
            while (name_end < tag.size() && tag[name_end] != '>' && tag[name_end] != '/' &&
                   std::isspace(static_cast<unsigned char>(tag[name_end])) == 0)
            {
                name_end++;
            }
            if (first != '/' && std::string_view(tag.data() + 1, name_end - 1) == "texture")
            {
                return 1;
            }
        }
        return 0;
    }

    bool parse_xml_uint(std::string_view value, uint64_t &out)
    {
        std::string decoded;
        decoded.reserve(std::min(value.size(), size_t{ 64 }));
        for (size_t pos = 0; pos < value.size();)
        {
            if (value[pos] != '&')
            {
                if (decoded.size() == 64)
                {
                    return false;
                }
                decoded.push_back(value[pos++]);
                continue;
            }
            if (pos + 3 >= value.size() || value[pos + 1] != '#')
            {
                return false;
            }
            size_t digit = pos + 2;
            int base = 10;
            if (value[digit] == 'x' || value[digit] == 'X')
            {
                base = 16;
                digit++;
            }
            const size_t semicolon = value.find(';', digit);
            if (semicolon == std::string_view::npos || semicolon == digit)
            {
                return false;
            }
            const std::string code_digits(value.substr(digit, semicolon - digit));
            char *end = nullptr;
            errno = 0;
            const unsigned long code = std::strtoul(code_digits.c_str(), &end, base);
            if (errno == ERANGE || end == nullptr || *end != '\0' || code > 0x7F || decoded.size() == 64)
            {
                return false;
            }
            decoded.push_back(static_cast<char>(code));
            pos = semicolon + 1;
        }

        const char *cursor = decoded.c_str();
        while (std::isspace(static_cast<unsigned char>(*cursor)) != 0)
        {
            cursor++;
        }
        if (*cursor == '+')
        {
            cursor++;
        }
        if (*cursor < '0' || *cursor > '9')
        {
            return false;
        }
        char *end = nullptr;
        errno = 0;
        out = std::strtoull(cursor, &end, 10);
        if (errno == ERANGE || end == nullptr)
        {
            return false;
        }
        while (std::isspace(static_cast<unsigned char>(*end)) != 0)
        {
            end++;
        }
        return *end == '\0';
    }

    bool amf_texture_dimensions_fit(std::string_view attributes, uintmax_t size)
    {
        uint64_t width = 0;
        uint64_t height = 0;
        uint64_t depth = 0;
        bool saw_width = false;
        bool saw_height = false;
        bool saw_depth = false;
        size_t pos = attributes.find_first_of(" \t\r\n/>", 1);
        if (pos == std::string_view::npos)
        {
            return false;
        }
        while (pos < attributes.size())
        {
            while (pos < attributes.size() && std::isspace(static_cast<unsigned char>(attributes[pos])) != 0)
            {
                pos++;
            }
            if (pos == attributes.size() || attributes[pos] == '/' || attributes[pos] == '>')
            {
                break;
            }
            const size_t name_start = pos;
            while (pos < attributes.size() && std::isspace(static_cast<unsigned char>(attributes[pos])) == 0 &&
                   attributes[pos] != '=' && attributes[pos] != '/' && attributes[pos] != '>')
            {
                pos++;
            }
            const std::string_view name = attributes.substr(name_start, pos - name_start);
            while (pos < attributes.size() && std::isspace(static_cast<unsigned char>(attributes[pos])) != 0)
            {
                pos++;
            }
            if (name.empty() || pos == attributes.size() || attributes[pos] != '=')
            {
                return false;
            }
            pos++;
            while (pos < attributes.size() && std::isspace(static_cast<unsigned char>(attributes[pos])) != 0)
            {
                pos++;
            }
            if (pos == attributes.size() || (attributes[pos] != '"' && attributes[pos] != '\''))
            {
                return false;
            }
            const char quote = attributes[pos++];
            const size_t value_start = pos;
            const size_t value_end = attributes.find(quote, value_start);
            if (value_end == std::string_view::npos)
            {
                return false;
            }
            const std::string_view value = attributes.substr(value_start, value_end - value_start);
            const auto set_dimension = [&](uint64_t &out, bool &seen)
            {
                if (seen || !parse_xml_uint(value, out))
                {
                    return false;
                }
                seen = true;
                return true;
            };
            if ((name == "width" && !set_dimension(width, saw_width)) ||
                (name == "height" && !set_dimension(height, saw_height)) ||
                (name == "depth" && !set_dimension(depth, saw_depth)))
            {
                return false;
            }
            pos = value_end + 1;
        }
        if (!saw_width || !saw_height || width == 0 || height == 0)
        {
            return true;
        }
        constexpr uint64_t u32_max = std::numeric_limits<uint32_t>::max();
        if (width > u32_max || height > u32_max || width > u32_max / height)
        {
            return false;
        }
        const uint64_t plane = width * height;
        return plane <= size && (!saw_depth || depth == 0 || (depth <= u32_max && depth <= size / plane));
    }

    // AMF multiplies texture dimensions in 32 bits and infers a missing depth by division.
    bool amf_declared_textures_fit_file(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return true;
        }
        const uintmax_t size = file_bytes(path);
        std::string tag;
        for (;;)
        {
            const int result = next_xml_texture_tag(in, tag);
            if (result <= 0)
            {
                return result == 0;
            }
            if (!amf_texture_dimensions_fit(tag, size))
            {
                return false;
            }
        }
    }

    struct LineHead
    {
        std::string keyword;
        uint64_t value = 0;
        bool has_value = false;
        bool overflow = false;
    };

    // Read a line's first word and unsigned value without retaining the rest of the line.
    bool read_line_head(std::istream &in, LineHead &out, size_t chunk_limit = std::numeric_limits<size_t>::max())
    {
        out = {};
        enum class State : uint8_t
        {
            Leading,
            Keyword,
            Gap,
            Digits,
            Rest,
        };
        State state = State::Leading;
        bool read_any = false;
        size_t line_chars = 0;
        char c = 0;
        while (in.get(c))
        {
            read_any = true;
            if (c == '\n' || c == '\r')
            {
                return true;
            }
            const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
            if (state == State::Leading)
            {
                if (!space)
                {
                    out.keyword.push_back(c);
                    state = State::Keyword;
                }
            }
            else if (state == State::Keyword)
            {
                if (space)
                {
                    state = State::Gap;
                }
                else if (out.keyword.size() <= 16)
                {
                    out.keyword.push_back(c);
                }
            }
            else if (state == State::Gap)
            {
                if (!space)
                {
                    if (c >= '0' && c <= '9')
                    {
                        out.has_value = true;
                        out.value = static_cast<uint64_t>(c - '0');
                        state = State::Digits;
                    }
                    else
                    {
                        state = State::Rest;
                    }
                }
            }
            else if (state == State::Digits)
            {
                if (c < '0' || c > '9')
                {
                    state = State::Rest;
                }
                else
                {
                    const auto digit = static_cast<uint64_t>(c - '0');
                    if (out.value > (std::numeric_limits<uint64_t>::max() - digit) / 10u)
                    {
                        out.overflow = true;
                    }
                    else if (!out.overflow)
                    {
                        out.value = (out.value * 10u) + digit;
                    }
                }
            }
            if (++line_chars == chunk_limit)
            {
                return true;
            }
        }
        return read_any;
    }

    // Bound MD5's unchecked vector resizes by the minimum line length for each item.
    bool md5_declared_counts_fit_file(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return true;
        }
        const uintmax_t size = file_bytes(path);
        LineHead line;
        while (read_line_head(in, line))
        {
            uint64_t minimum_line_bytes = 0;
            if (line.keyword == "numverts")
            {
                minimum_line_bytes = 12;
            }
            else if (line.keyword == "numtris")
            {
                minimum_line_bytes = 10;
            }
            else if (line.keyword == "numweights")
            {
                minimum_line_bytes = 16;
            }
            else
            {
                continue;
            }
            if (line.overflow || (line.has_value && line.value > size / minimum_line_bytes))
            {
                return false;
            }
        }
        return in.eof();
    }

    // NFF tessellation and AC3D subdivision grow by 4^N with no useful upstream bound.
    bool ac_subdivision_within_bounds(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return true;
        }
        LineHead line;
        while (read_line_head(in, line))
        {
            if (line.keyword != "subdiv")
            {
                continue;
            }
            if (line.overflow || (line.has_value && line.value > 6))
            {
                return false;
            }
        }
        return in.eof();
    }

    bool nff_tessellation_within_bounds(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return true;
        }
        LineHead line;
        while (read_line_head(in, line, 4095))
        {
            if (line.keyword != "tess")
            {
                continue;
            }
            if (line.overflow || (line.has_value && line.value > 8))
            {
                return false;
            }
        }
        return in.eof();
    }

    // Drive-relative paths resolve against the model directory.
    bool absolute_path(const std::string &path)
    {
        return (!path.empty() && path[0] == '/') ||
               (path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
                path[1] == ':' && path[2] == '/') ||
               (path.size() >= 2 && path[0] == '/' && path[1] == '/');
    }

    WrapMode to_wrap(aiTextureMapMode mode)
    {
        switch (mode)
        {
        case aiTextureMapMode_Clamp:
        case aiTextureMapMode_Decal:
            return WrapMode::Clamp;
        case aiTextureMapMode_Mirror:
            return WrapMode::Mirror;
        case aiTextureMapMode_Wrap:
        default:
            return WrapMode::Repeat;
        }
    }

    bool read_file(const std::string &path, std::vector<uint8_t> &bytes)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            return false;
        }
        const std::streamoff end = in.tellg();
        if (end <= 0 || static_cast<uintmax_t>(end) > bytes.max_size() ||
            static_cast<uintmax_t>(end) > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        {
            return false;
        }
        bytes.resize(static_cast<size_t>(end));
        in.seekg(0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return static_cast<bool>(in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(end)));
    }

    bool decode_bytes(Texture &texture, const uint8_t *data, size_t size, const std::string &hint)
    {
        if (is_ktx2(data, size) || hint == "ktx2")
        {
            return texture.load_ktx2_from_memory(data, size);
        }
        if (is_webp(data, size) || hint == "webp")
        {
            return texture.load_webp_from_memory(data, size);
        }
        return texture.load_from_memory(data, size);
    }

    struct TextureSource
    {
        const aiTexture *embedded = nullptr;
        std::string path;
        std::string identity;
        std::string hint;
        WrapMode wrap_s = WrapMode::Repeat;
        WrapMode wrap_t = WrapMode::Repeat;
        uint8_t uv_set = 0;
        bool height_map = false;
        bool maybe_height = false;
        bool amf_origin = false;
        bool valid = false;
    };

    TextureSource texture_source(
        const aiScene *scene,
        const aiMaterial *material,
        aiTextureType type,
        const std::string &model_dir,
        const std::string &extension,
        bool height_map = false,
        bool maybe_height = false
    )
    {
        aiString raw_path;
        aiTextureMapping mapping = aiTextureMapping_UV;
        unsigned int uv = 0;
        aiTextureMapMode modes[3] = { aiTextureMapMode_Wrap, aiTextureMapMode_Wrap, aiTextureMapMode_Wrap };
        unsigned int tex_index = 0;
        for (;;)
        {
            if (material->GetTexture(type, tex_index, &raw_path, &mapping, &uv, nullptr, nullptr, modes) !=
                    AI_SUCCESS ||
                mapping != aiTextureMapping_UV || raw_path.length == 0)
            {
                return {};
            }
            // Blender procedural sentinels can precede a usable image slot.
            if (extension != ".blend" || std::strncmp(raw_path.C_Str(), "Procedural,", 11) != 0)
            {
                break;
            }
            if (++tex_index > 24)
            {
                return {};
            }
        }

        TextureSource out;
        std::string authored = normalized_path(raw_path.C_Str());
        // Assimp joins X3D's fallback URL list as first""second. X3D uses the first URL.
        if (extension == ".x3d" || extension == ".x3db")
        {
            const size_t join = authored.find("\"\"");
            if (join != std::string::npos)
            {
                authored.resize(join);
            }
        }
        // GetEmbeddedTexture parses every leading '*' as a numeric index. 3MF also uses
        // star-prefixed paths, which must instead match by filename.
        const bool numeric_star_ref =
            authored.size() > 1 && authored[0] == '*' &&
            std::all_of(authored.begin() + 1, authored.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        out.embedded = scene->GetEmbeddedTexture(
            numeric_star_ref ? authored.c_str() : authored.c_str() + (authored[0] == '*' ? 1 : 0)
        );
        // Blender's // prefix is model-relative, not UNC.
        if (extension == ".blend" && authored.rfind("//", 0) == 0)
        {
            authored.erase(0, 2);
        }
        out.path = out.embedded || absolute_path(authored) ? authored : model_dir + authored;
        out.identity = (out.embedded ? "embedded:" : "file:") + authored;
        out.hint = out.embedded ? assimp_detail::embedded_format_hint(*out.embedded) : std::string();
        // X3D and AMF store booleans in an enum where 1 means Clamp, reversing their intent.
        if (extension == ".x3d" || extension == ".x3db" || extension == ".amf")
        {
            modes[0] = modes[0] == aiTextureMapMode_Clamp ? aiTextureMapMode_Wrap : aiTextureMapMode_Clamp;
            modes[1] = modes[1] == aiTextureMapMode_Clamp ? aiTextureMapMode_Wrap : aiTextureMapMode_Clamp;
        }
        out.wrap_s = to_wrap(modes[0]);
        out.wrap_t = to_wrap(modes[1]);
        out.uv_set = uv >= 1 ? 1 : 0;
        out.height_map = height_map;
        out.maybe_height = maybe_height;
        out.amf_origin = extension == ".amf";
        out.valid = true;
        return out;
    }

    std::string texture_key(const TextureSource &source)
    {
        return source.identity + "|" + std::to_string(static_cast<int>(source.wrap_s)) + "|" +
               std::to_string(static_cast<int>(source.wrap_t)) + "|" + (source.height_map ? "height" : "color") + "|" +
               (source.maybe_height ? "maybe-height" : "exact") + "|" + (source.amf_origin ? "amf" : "native");
    }

    bool same_texture_binding(const TextureSource &a, const TextureSource &b)
    {
        return a.valid && b.valid && a.identity == b.identity && a.wrap_s == b.wrap_s && a.wrap_t == b.wrap_t &&
               a.uv_set == b.uv_set;
    }

    Texture decode_texture_impl(const TextureSource &source)
    {
        Texture texture;
        if (source.embedded)
        {
            const aiTexture &embedded = *source.embedded;
            if (!embedded.pcData)
            {
                return texture;
            }
            if (embedded.mHeight == 0)
            {
                // Compressed aiTexture data is a byte buffer behind an aiTexel pointer.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                const auto *bytes = reinterpret_cast<const uint8_t *>(embedded.pcData);
                (void)decode_bytes(texture, bytes, embedded.mWidth, source.hint);
            }
            else
            {
                const size_t width = embedded.mWidth;
                const size_t height = embedded.mHeight;
                if (width == 0 || width > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                    height > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                    width > std::numeric_limits<size_t>::max() / height ||
                    width * height > std::numeric_limits<size_t>::max() / 4)
                {
                    return texture;
                }
                texture.width = static_cast<int>(width);
                texture.height = static_cast<int>(height);
                texture.pixels.resize(width * height * 4);
                const char *hint_end =
                    static_cast<const char *>(std::memchr(embedded.achFormatHint, '\0', sizeof embedded.achFormatHint));
                const std::string_view authored(
                    std::begin(embedded.achFormatHint), hint_end == nullptr
                                                            ? sizeof embedded.achFormatHint
                                                            : static_cast<size_t>(hint_end - embedded.achFormatHint)
                );
                const std::string_view hint = authored.substr(0, 8);
                if (embedded_texel_layout(hint, source.amf_origin) == TexelLayout::ArgbTexels)
                {
                    for (size_t i = 0; i < width * height; i++)
                    {
                        const aiTexel &src = embedded.pcData[i];
                        texture.pixels[i * 4] = src.r;
                        texture.pixels[(i * 4) + 1] = src.g;
                        texture.pixels[(i * 4) + 2] = src.b;
                        texture.pixels[(i * 4) + 3] = src.a;
                    }
                }
                else
                {
                    const auto stride = static_cast<size_t>(std::count(hint.begin() + 4, hint.end(), '8'));
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                    const auto *bytes = reinterpret_cast<const uint8_t *>(embedded.pcData);
                    for (size_t i = 0; i < width * height; i++)
                    {
                        uint8_t *out = &texture.pixels[i * 4];
                        out[3] = std::numeric_limits<uint8_t>::max();
                        size_t src_channel = 0;
                        for (size_t c = 0; c < 4; c++)
                        {
                            if (hint[4 + c] == '8')
                            {
                                out[c] = bytes[(i * stride) + src_channel++];
                            }
                        }
                    }
                }
            }
        }
        else
        {
            std::vector<uint8_t> bytes;
            if (!read_file(source.path, bytes))
            {
                return texture;
            }
            (void)decode_bytes(texture, bytes.data(), bytes.size(), source.hint);
        }

        if (!texture.valid())
        {
            return texture;
        }
        texture.wrap_s = source.wrap_s;
        texture.wrap_t = source.wrap_t;
        if (source.height_map)
        {
            // Colored height data cannot be decoded as signed XYZ normals.
            if (is_grayscale(texture))
            {
                texture = height_to_normal_map(texture, 'l', 1.0f);
            }
            else
            {
                return {};
            }
        }
        else if (source.maybe_height && is_grayscale(texture))
        {
            texture = height_to_normal_map(texture, 'l', 1.0f);
        }
        return texture;
    }

    Texture decode_texture(const TextureSource &source)
    {
        // Worker exceptions cannot reach load_model's outer guard.
        try
        {
            return decode_texture_impl(source);
        }
        catch (const std::bad_alloc &)
        {
            return {};
        }
        catch (const std::length_error &)
        {
            return {};
        }
    }

} // namespace

namespace assimp_detail
{
    std::string embedded_format_hint(const aiTexture &source)
    {
        const char *const begin = source.achFormatHint;
        constexpr size_t max_length = sizeof(source.achFormatHint) - 1;
        const auto *const end = static_cast<const char *>(std::memchr(begin, '\0', max_length));
        return { begin, end == nullptr ? max_length : static_cast<size_t>(end - begin) };
    }

    bool get_blend_func(const aiMaterial &source, int &out)
    {
        for (unsigned int i = 0; i < source.mNumProperties; ++i)
        {
            const aiMaterialProperty *prop = source.mProperties[i];
            if (prop == nullptr || prop->mSemantic != 0 || prop->mIndex != 0 || prop->mData == nullptr)
            {
                continue;
            }
            if (std::strcmp(prop->mKey.C_Str(), "$mat.blend") != 0)
            {
                continue;
            }
            if ((prop->mType != aiPTI_Integer && prop->mType != aiPTI_Buffer) || prop->mDataLength < sizeof(int32_t))
            {
                return false;
            }
            int32_t value = 0;
            std::memcpy(&value, prop->mData, sizeof value);
            out = static_cast<int>(value);
            return true;
        }
        return false;
    }
} // namespace assimp_detail

bool Mesh::load_assimp(const std::string &path, int n_threads, float crease_angle_deg)
{
    MeshSnapshot snapshot(*this);

    std::string extension = lower_extension(path);
    if (extension.empty())
    {
        return false;
    }

    install_assert_handler();

    Assimp::Importer importer;
    // Ogre registers the two-part .mesh.xml extension.
    if (!importer.IsExtensionSupported(extension))
    {
        const size_t extension_pos = path.find_last_of('.');
        if (extension_pos > 0)
        {
            const std::string two = lower_extension(path.substr(0, extension_pos));
            if (!two.empty() && importer.IsExtensionSupported(two))
            {
                extension = two;
            }
            else
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }

    // These loaders ignore AI_CONFIG_IMPORT_NO_SKELETON_MESHES.
    if (extension == ".md5anim" || extension == ".md5camera")
    {
        return false;
    }

    const float smoothing_angle = clamp(crease_angle_deg, 0.0f, 175.0f);
    importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, smoothing_angle);
    // Reject motion-only files instead of synthesizing stick figures.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_NO_SKELETON_MESHES, true);
    // Animation sidecars are unused and can make an otherwise valid mesh fail.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_MD5_NO_ANIM_AUTOLOAD, true);
    // Resolve an MDL colormap beside the model before Assimp's built-in fallback.
    if (extension == ".mdl")
    {
        importer.SetPropertyString(AI_CONFIG_IMPORT_MDL_COLORMAP, normalized_path(dir_of(path)) + "colormap.lmp");
    }
    // Preserve Unreal's authored two-sided and translucent face flags.
    if (extension == ".3d" || extension == ".uc")
    {
        importer.SetPropertyBool(AI_CONFIG_IMPORT_UNREAL_HANDLE_FLAGS, false);
    }
    // Prefer ASE's authored normals.
    if (extension == ".ase" || extension == ".ask")
    {
        importer.SetPropertyInteger(AI_CONFIG_IMPORT_ASE_RECONSTRUCT_NORMALS, 0);
    }
    unsigned int flags = aiProcess_Triangulate | aiProcess_SortByPType | aiProcess_PreTransformVertices |
                         aiProcess_JoinIdenticalVertices | aiProcess_ValidateDataStructure | aiProcess_GenUVCoords |
                         aiProcess_TransformUVCoords;
    // X3D stores one-byte booleans where TextureTransformStep reads four-byte enums.
    if (extension == ".x3d" || extension == ".x3db")
    {
        flags &= ~aiProcess_TransformUVCoords;
    }
    // Importers that need UV or winding conversion already do it. The remaining format
    // fixes run after the vertex copy.
    flags |= crease_angle_deg <= 0.0f ? aiProcess_GenNormals : aiProcess_GenSmoothNormals;

    const std::string model_dir = normalized_path(dir_of(path));

    if (extension == ".amf" && !amf_declared_textures_fit_file(path))
    {
        std::fprintf(stderr, "note: declared texture size in '%s' exceeds the file size\n", path.c_str());
        return false;
    }
    if (extension == ".ter" && !terragen_grid_fits_file(path))
    {
        std::fprintf(stderr, "note: declared terrain grid exceeds the size of '%s'\n", path.c_str());
        return false;
    }
    if (extension == ".md5mesh" && !md5_declared_counts_fit_file(path))
    {
        std::fprintf(stderr, "note: declared element counts in '%s' exceed the file size\n", path.c_str());
        return false;
    }
    if ((extension == ".nff" || extension == ".enff") && !nff_tessellation_within_bounds(path))
    {
        std::fprintf(stderr, "note: tessellation level in '%s' exceeds the supported maximum (8)\n", path.c_str());
        return false;
    }
    if (extension == ".off" && !off_declared_counts_fit_file(path))
    {
        std::fprintf(stderr, "note: declared vertex or face count in '%s' exceeds the file size\n", path.c_str());
        return false;
    }
    if ((extension == ".ac" || extension == ".acc" || extension == ".ac3d") && !ac_subdivision_within_bounds(path))
    {
        std::fprintf(stderr, "note: subdivision level in '%s' exceeds the supported maximum (6)\n", path.c_str());
        return false;
    }
    if ((extension == ".3d" || extension == ".uc") && !unreal_triangle_indices_in_bounds(path))
    {
        std::fprintf(stderr, "note: triangle vertex index in '%s' exceeds the vertex count\n", path.c_str());
        return false;
    }

    const aiScene *scene = importer.ReadFile(path, flags);
    if (!scene || !scene->mRootNode || scene->mNumMeshes == 0)
    {
        if (const char *error = importer.GetErrorString(); error && *error)
        {
            std::fprintf(stderr, "note: Assimp could not load '%s': %s\n", path.c_str(), error);
        }
        return false;
    }
    if (scene->mNumMaterials >= std::numeric_limits<uint32_t>::max())
    {
        return false;
    }

    materials.reserve(static_cast<size_t>(scene->mNumMaterials) + 1);
    materials.push_back(Material{});
    // Drop texture bindings if every mesh lacks UVs. RAW otherwise samples texel 0,0.
    bool any_uv_channel = false;
    // Disabling Unreal's flag handling exposes its weapon-attachment placeholder.
    const bool unreal_weapon_placeholders = extension == ".3d" || extension == ".uc";
    auto is_weapon_placeholder = [&](const aiMesh *mesh)
    {
        if (!unreal_weapon_placeholders || mesh->mMaterialIndex >= scene->mNumMaterials)
        {
            return false;
        }
        aiString name;
        return scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, name) == AI_SUCCESS &&
               std::strcmp(name.C_Str(), "$WeaponTag$") == 0;
    };
    for (unsigned int i = 0; i < scene->mNumMeshes; i++)
    {
        const aiMesh *mesh = scene->mMeshes[i];
        if (is_weapon_placeholder(mesh))
        {
            continue;
        }
        any_uv_channel = any_uv_channel || mesh->HasTextureCoords(0) || mesh->HasTextureCoords(1);
    }
    std::unordered_map<std::string, int> texture_cache;
    std::vector<TextureSource> texture_requests;
    auto register_texture = [&](const TextureSource &source) -> TexSlot
    {
        TexSlot slot;
        if (!source.valid || !any_uv_channel)
        {
            return slot;
        }
        if (texture_requests.size() >= static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return slot;
        }
        slot.uv_set = source.uv_set;
        const std::string key = texture_key(source);
        const auto found = texture_cache.find(key);
        if (found != texture_cache.end())
        {
            slot.tex = found->second;
            return slot;
        }
        slot.tex = static_cast<int>(texture_requests.size());
        texture_requests.push_back(source);
        texture_cache.emplace(key, slot.tex);
        return slot;
    };

    for (unsigned int i = 0; i < scene->mNumMaterials; i++)
    {
        const aiMaterial *source = scene->mMaterials[i];
        Material material;
        int integer = 0;
        aiColor4D color;
        aiReturn color_result = source->Get(AI_MATKEY_BASE_COLOR, color);
        if (color_result != AI_SUCCESS)
        {
            color_result = source->Get(AI_MATKEY_COLOR_DIFFUSE, color);
        }
        if (color_result != AI_SUCCESS && extension == ".blend")
        {
            // Legacy Blender omits COLOR_DIFFUSE for black and keeps it in a private key.
            aiColor3D blend_color;
            if (source->Get("$mat.blend.diffuse.color", 0, 0, blend_color) == AI_SUCCESS)
            {
                color = aiColor4D(blend_color.r, blend_color.g, blend_color.b, 1.0f);
                color_result = AI_SUCCESS;
            }
        }
        if (color_result == AI_SUCCESS && finite(color))
        {
            material.diffuse = to_vec3(color);
            // DXF and Irrlicht use synthetic or absent diffuse alpha. Zero means opaque.
            float alpha = unit(color.a);
            const bool synthetic_zero_alpha = extension == ".dxf" || extension == ".irr" || extension == ".irrmesh";
            if (synthetic_zero_alpha && alpha == 0.0f)
            {
                alpha = 1.0f;
            }
            material.alpha = alpha;
        }
        material.ambient = material.diffuse;
        aiColor3D color3;
        // Ignore importer template ambients that make Flat lighting nearly black.
        bool synthetic_ambient =
            extension == ".dxf" || extension == ".md2" || extension == ".md3" || extension == ".mdc";
        if (extension == ".mdl" || extension == ".hmp" || extension == ".ase")
        {
            aiString material_name;
            synthetic_ambient = source->Get(AI_MATKEY_NAME, material_name) != AI_SUCCESS ||
                                std::strcmp(material_name.C_Str(), "DefaultMaterial") == 0;
        }
        if (!synthetic_ambient && source->Get(AI_MATKEY_COLOR_AMBIENT, color3) == AI_SUCCESS && finite(color3.r) &&
            finite(color3.g) && finite(color3.b))
        {
            // NFF parses scalar Ka into red only.
            if ((extension == ".nff" || extension == ".enff") && color3.g == 0.0f && color3.b == 0.0f &&
                color3.r > 0.0f)
            {
                color3.g = color3.b = color3.r;
            }
            // All-zero and known template ambients mean absent.
            const bool collada_default_ambient =
                ((extension == ".dae" && color3.r == 0.1f && color3.g == 0.1f && color3.b == 0.1f) ||
                 // X3D defaults ambientIntensity to 0.2.
                 ((extension == ".x3d" || extension == ".x3db") && color3.r == 0.2f && color3.g == 0.2f &&
                  color3.b == 0.2f));
            const bool ambient_zero = (color3.r == 0.0f && color3.g == 0.0f && color3.b == 0.0f);
            if (!ambient_zero && !collada_default_ambient)
            {
                material.ambient = to_vec3(color3);
            }
        }
        // Ignore hardcoded specular values from importer template materials.
        bool synthetic_specular =
            extension == ".md2" || extension == ".md3" || extension == ".dxf" || extension == ".mdc";
        if (extension == ".mdl" || extension == ".hmp" || extension == ".ase")
        {
            aiString material_name;
            const bool template_material = source->Get(AI_MATKEY_NAME, material_name) != AI_SUCCESS ||
                                           std::strcmp(material_name.C_Str(), "DefaultMaterial") == 0;
            synthetic_specular = synthetic_specular || template_material;
        }
        if (!synthetic_specular && source->Get(AI_MATKEY_COLOR_SPECULAR, color3) == AI_SUCCESS && finite(color3.r) &&
            finite(color3.g) && finite(color3.b))
        {
            const bool collada_default_specular =
                extension == ".dae" && color3.r == 0.4f && color3.g == 0.4f && color3.b == 0.4f;
            if (!collada_default_specular)
            {
                material.specular = to_vec3(color3);
            }
        }
        if (source->Get(AI_MATKEY_COLOR_EMISSIVE, color3) == AI_SUCCESS && finite(color3.r) && finite(color3.g) &&
            finite(color3.b))
        {
            material.emissive = to_vec3(color3);
            float emissive_intensity = 0.0f;
            if (source->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissive_intensity) == AI_SUCCESS &&
                finite(emissive_intensity) && emissive_intensity > 0.0f)
            {
                material.emissive *= emissive_intensity;
            }
        }

        int blend_func = 0;
        const bool has_blend_func = assimp_detail::get_blend_func(*source, blend_func);
        ai_real scalar = 0.0f;
        // LWO's additive surfaces carry their ADTR glow amount in OPACITY; applied as
        // alpha-OVER opacity it just renders them wrongly translucent.
        const bool additive_opacity = (extension == ".lwo" || extension == ".lxo" || extension == ".lws") &&
                                      has_blend_func && blend_func == aiBlendMode_Additive;
        // HMP/MDL7 write OPACITY from an often-uninitialized ambient alpha.
        const bool synthetic_zero_opacity = extension == ".hmp" || extension == ".mdl";
        if (!additive_opacity && source->Get(AI_MATKEY_OPACITY, scalar) == AI_SUCCESS && finite(scalar) &&
            (scalar > 0.0f || has_blend_func || !synthetic_zero_opacity))
        {
            material.alpha *= unit(scalar);
        }
        bool have_shininess = false;
        if (source->Get(AI_MATKEY_SHININESS, scalar) == AI_SUCCESS && finite(scalar) && scalar >= 0.0f)
        {
            // Collada stamps its template SHININESS of 10 into every material that authors
            // none; treating it as absent keeps such materials at our exponent. The gate
            // must sit AFTER the read above overwrites scalar, or it judges stale OPACITY.
            const bool collada_template_shininess = extension == ".dae" && scalar == 10.0f;
            if (!collada_template_shininess)
            {
                material.shininess = scalar;
                have_shininess = true;
            }
        }
        if ((extension == ".ifc" || extension == ".ifczip" || extension == ".step" || extension == ".stp") &&
            have_shininess && material.shininess < 2.0f)
        {
            // IFC reads IfcSpecularExponent and IfcSpecularRoughness into the same float,
            // so a roughness-style value like 0.2 arrives as a near-zero exponent and
            // shades the whole surface uniformly. A real exponent that small is useless
            // anyway; interpret sub-2 values as roughness.
            material.shininess = roughness_to_shininess(unit(material.shininess));
        }
        if (have_shininess)
        {
            // Collada and X3D write zero for an authored matte material.
            material.shininess = std::max(material.shininess, roughness_to_shininess(1.0f));
        }
        if (source->Get(AI_MATKEY_SHININESS_STRENGTH, scalar) == AI_SUCCESS && finite(scalar))
        {
            // PMX stores an exponent here. Other importers store a strength multiplier.
            if (extension == ".pmx")
            {
                if (!have_shininess && scalar > 0.0f)
                {
                    material.shininess = scalar;
                    have_shininess = true;
                }
            }
            else
            {
                material.specular *= unit(scalar);
            }
        }
        // Blender's synthetic DefaultMaterial has uninitialized private fields.
        bool is_blend_default_material = false;
        if (extension == ".blend")
        {
            aiString material_name;
            is_blend_default_material = source->Get(AI_MATKEY_NAME, material_name) == AI_SUCCESS &&
                                        std::strcmp(material_name.C_Str(), "DefaultMaterial") == 0;
        }
        if (extension == ".blend" && !is_blend_default_material)
        {
            // Legacy Blender Internal stores transparency in private keys.
            int transparency_used = 0;
            if (source->Get("$mat.blend.transparency.use", 0, 0, transparency_used) == AI_SUCCESS &&
                transparency_used != 0 && source->Get("$mat.blend.transparency.alpha", 0, 0, scalar) == AI_SUCCESS &&
                finite(scalar) && scalar > 0.0f)
            {
                material.alpha *= unit(scalar);
            }
            if (source->Get("$mat.blend.diffuse.intensity", 0, 0, scalar) == AI_SUCCESS && finite(scalar) &&
                scalar >= 0.0f)
            {
                material.diffuse *= scalar;
            }
            if (source->Get("$mat.blend.specular.intensity", 0, 0, scalar) == AI_SUCCESS && finite(scalar) &&
                scalar >= 0.0f)
            {
                material.specular *= scalar;
            }
        }
        if ((extension == ".irr" || extension == ".irrmesh" || extension == ".nff" || extension == ".enff") &&
            source->Get(AI_MATKEY_SHADING_MODEL, integer) == AI_SUCCESS && integer == aiShadingMode_NoShading)
        {
            // These importers write NoShading only when the file requests it.
            material.unlit = true;
        }
        bool metallic_authored = false;
        if (source->Get(AI_MATKEY_METALLIC_FACTOR, scalar) == AI_SUCCESS && finite(scalar))
        {
            metallic_authored = true;
            material.metallic = unit(scalar);
        }
        if (source->Get(AI_MATKEY_ROUGHNESS_FACTOR, scalar) == AI_SUCCESS && finite(scalar))
        {
            material.roughness = unit(scalar);
            // Classic FBX derives roughness from authored shininess. Metallic marks PBR
            // data, where the shininess key is only a template default.
            if (!have_shininess || metallic_authored)
            {
                material.shininess = roughness_to_shininess(material.roughness);
            }
        }
        if (source->Get(AI_MATKEY_TWOSIDED, integer) == AI_SUCCESS)
        {
            material.double_sided = integer != 0;
        }
        if (extension == ".irr")
        {
            // Irrlicht skybox faces are visible from inside.
            aiString material_name;
            if (source->Get(AI_MATKEY_NAME, material_name) == AI_SUCCESS &&
                std::strncmp(material_name.C_Str(), "SkyboxSide_", 11) == 0)
            {
                material.double_sided = true;
            }
        }
        TextureSource diffuse = texture_source(scene, source, aiTextureType_BASE_COLOR, model_dir, extension);
        if (!diffuse.valid)
        {
            diffuse = texture_source(scene, source, aiTextureType_DIFFUSE, model_dir, extension);
        }
        material.diffuse_map = register_texture(diffuse);
        TexSlot specular_slot =
            register_texture(texture_source(scene, source, aiTextureType_SPECULAR, model_dir, extension));
        if (specular_slot.tex < 0 && (extension == ".mesh" || extension == ".mesh.xml"))
        {
            // Ogre's "$specular_map" uses SHININESS. Elsewhere that type means gloss.
            specular_slot =
                register_texture(texture_source(scene, source, aiTextureType_SHININESS, model_dir, extension));
        }
        material.specular_map = specular_slot;
        TextureSource normal = texture_source(scene, source, aiTextureType_NORMALS, model_dir, extension);
        if (!normal.valid)
        {
            // FBX presets bind normal maps to the camera-space slot.
            normal = texture_source(scene, source, aiTextureType_NORMAL_CAMERA, model_dir, extension);
        }
        if (!normal.valid)
        {
            normal = texture_source(scene, source, aiTextureType_HEIGHT, model_dir, extension, true);
        }
        else if (extension == ".dae")
        {
            // Collada routes grayscale <bump> textures through NORMALS.
            normal.maybe_height = true;
        }
        material.normal_map = register_texture(normal);
        TextureSource emissive = texture_source(scene, source, aiTextureType_EMISSIVE, model_dir, extension);
        if (!emissive.valid)
        {
            emissive = texture_source(scene, source, aiTextureType_EMISSION_COLOR, model_dir, extension);
        }
        material.emissive_map = register_texture(emissive);
        material.occlusion_map =
            register_texture(texture_source(scene, source, aiTextureType_AMBIENT_OCCLUSION, model_dir, extension));

        const TextureSource metal = texture_source(scene, source, aiTextureType_METALNESS, model_dir, extension);
        const TextureSource rough =
            texture_source(scene, source, aiTextureType_DIFFUSE_ROUGHNESS, model_dir, extension);
        if (same_texture_binding(metal, rough))
        {
            material.mr_map = register_texture(metal);
        }
        if (extension == ".3mf" && diffuse.valid && material.diffuse.x == 0.0f && material.diffuse.y == 0.0f &&
            material.diffuse.z == 0.0f)
        {
            // 3MF supplies black template colors for textured materials.
            const vec3 white = { 1.0f, 1.0f, 1.0f };
            material.diffuse = white;
            material.ambient = white;
            material.specular = { 0.4f, 0.4f, 0.4f };
        }

        // Texture alpha alone does not select the transparent pass.
        material.blend = material.alpha < 1.0f;
        // MD3 uses BLEND_FUNC without OPACITY. LWO stamps the same key on every surface,
        // so it cannot enable blending globally. Additive blending remains unsupported.
        if (extension == ".md3" && has_blend_func && blend_func == aiBlendMode_Default)
        {
            material.blend = true;
        }
        materials.push_back(material);
    }

    decode_textures(
        textures, materials, texture_requests.size(), n_threads,
        [&](size_t i) { return decode_texture(texture_requests[i]); }
    );
    const bool any_uv1_reference = std::any_of(
        materials.begin(), materials.end(),
        [](const Material &material)
        {
            const auto uses_uv1 = [](const TexSlot &slot) { return slot.tex >= 0 && slot.uv_set == 1; };
            return uses_uv1(material.diffuse_map) || uses_uv1(material.specular_map) || uses_uv1(material.normal_map) ||
                   uses_uv1(material.emissive_map) || uses_uv1(material.occlusion_map) || uses_uv1(material.mr_map);
        }
    );

    bool any_colors = false;
    bool any_alpha = false;
    // Legacy Blender MCol vertex colors arrive as raw SIGNED chars, so opaque 255 reads
    // as -1 and the whole mesh would route into the blend pass invisible. Signed values
    // carry no usable data; drop colors entirely when one shows up.
    bool blend_colors_garbage = false;
    bool off_integer_colors = false;
    uint64_t total_vertices = 0;
    uint64_t total_triangles = 0;
    for (unsigned int i = 0; i < scene->mNumMeshes; i++)
    {
        const aiMesh *mesh = scene->mMeshes[i];
        if (!(mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) || is_weapon_placeholder(mesh))
        {
            continue;
        }
        total_vertices += mesh->mNumVertices;
        total_triangles += mesh->mNumFaces;
        if (total_vertices > std::numeric_limits<uint32_t>::max() ||
            total_triangles > std::numeric_limits<uint32_t>::max())
        {
            return false;
        }
        any_colors = any_colors || mesh->HasVertexColors(0);
        if (mesh->HasVertexColors(0))
        {
            for (unsigned int v = 0; v < mesh->mNumVertices; v++)
            {
                const aiColor4D &vertex_color = mesh->mColors[0][v];
                if (!finite(vertex_color))
                {
                    return false;
                }
                blend_colors_garbage =
                    blend_colors_garbage || (extension == ".blend" && (vertex_color.r < 0.0f || vertex_color.g < 0.0f ||
                                                                       vertex_color.b < 0.0f || vertex_color.a < 0.0f));
                const bool amf_default_color = extension == ".amf" && vertex_color.r == 0.0f &&
                                               vertex_color.g == 0.0f && vertex_color.b == 0.0f &&
                                               vertex_color.a == 0.0f;
                // COFF may use 0..255. Irrlicht and 3MF leave alpha at zero for RGB-only colors.
                const bool alpha_authored = [&]
                {
                    if (extension == ".off")
                    {
                        return vertex_color.a != 1.0f && vertex_color.a != 255.0f;
                    }
                    if ((extension == ".irr" || extension == ".irrmesh"))
                    {
                        return vertex_color.a > 0.0f && vertex_color.a < 1.0f;
                    }
                    if (extension == ".3mf")
                    {
                        return vertex_color.a > 0.0f && vertex_color.a < 1.0f;
                    }
                    return vertex_color.a < 1.0f;
                }();
                // DXF stamps synthetic alpha on uncolored vertices.
                any_alpha = any_alpha || (extension != ".dxf" && !amf_default_color && alpha_authored);
                // A COFF channel above one selects the format's 0..255 interpretation.
                if (extension == ".off" && (vertex_color.r > 1.0f || vertex_color.g > 1.0f || vertex_color.b > 1.0f))
                {
                    off_integer_colors = true;
                }
            }
        }
    }
    has_vertex_colors = any_colors && !blend_colors_garbage;
    has_vertex_alpha = any_alpha && !blend_colors_garbage;
    // Duplicate UV0 when a material requests a second set the importer omitted.
    has_uv1 = any_uv1_reference;
    vertices.reserve(static_cast<size_t>(total_vertices));
    bool saw_zero_normal = false;
    triangles.reserve(static_cast<size_t>(total_triangles));
    if (has_vertex_colors)
    {
        vertex_colors.reserve(static_cast<size_t>(total_vertices));
    }
    if (has_vertex_alpha)
    {
        vertex_alpha.reserve(static_cast<size_t>(total_vertices));
    }
    if (has_uv1)
    {
        uv1.reserve(static_cast<size_t>(total_vertices));
    }

    for (unsigned int i = 0; i < scene->mNumMeshes; i++)
    {
        const aiMesh *source = scene->mMeshes[i];
        if (!(source->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) || is_weapon_placeholder(source))
        {
            continue;
        }
        const auto base = static_cast<uint32_t>(vertices.size());
        for (unsigned int v = 0; v < source->mNumVertices; v++)
        {
            const aiVector3D position = source->mVertices[v];
            const aiVector3D normal = source->HasNormals() ? source->mNormals[v] : aiVector3D(0.0f, 1.0f, 0.0f);
            // Assimp leaves zero or non-finite normals at degenerate and sharp vertices.
            // Refill them below, while still rejecting invalid positions and UVs.
            const bool usable_normal = source->HasNormals() && finite(normal) &&
                                       (normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z) > 0.0f;
            if (!usable_normal)
            {
                saw_zero_normal = true;
            }
            const aiVector3D texcoord = source->HasTextureCoords(0) ? source->mTextureCoords[0][v] : aiVector3D();
            if (!finite(position) || !finite(texcoord))
            {
                return false;
            }
            vertices.push_back({ { position.x, position.y, position.z },
                                 usable_normal ? vec3(normal.x, normal.y, normal.z) : vec3(),
                                 { texcoord.x, texcoord.y },
                                 1.0f });
            aiColor4D vertex_color = source->HasVertexColors(0) ? source->mColors[0][v] : aiColor4D(1.0f);
            if (extension == ".amf" && vertex_color.r == 0.0f && vertex_color.g == 0.0f && vertex_color.b == 0.0f &&
                vertex_color.a == 0.0f)
            {
                vertex_color = aiColor4D(1.0f);
            }
            else if ((extension == ".irr" || extension == ".irrmesh" || extension == ".3mf") && vertex_color.a == 0.0f)
            {
                vertex_color.a = 1.0f;
            }
            const float color_scale = off_integer_colors ? 1.0f / 255.0f : 1.0f;
            if (has_vertex_colors)
            {
                vertex_colors.emplace_back(
                    vertex_color.r * color_scale, vertex_color.g * color_scale, vertex_color.b * color_scale
                );
            }
            if (has_vertex_alpha)
            {
                vertex_alpha.push_back(unit(vertex_color.a * color_scale));
            }
            if (has_uv1)
            {
                const aiVector3D second = source->HasTextureCoords(1) ? source->mTextureCoords[1][v] : texcoord;
                if (!finite(second))
                {
                    return false;
                }
                uv1.emplace_back(second.x, second.y);
            }
        }

        const uint32_t material = source->mMaterialIndex < scene->mNumMaterials ? source->mMaterialIndex + 1 : 0;
        for (unsigned int f = 0; f < source->mNumFaces; f++)
        {
            const aiFace &face = source->mFaces[f];
            if (face.mNumIndices != 3 || !face.mIndices || face.mIndices[0] >= source->mNumVertices ||
                face.mIndices[1] >= source->mNumVertices || face.mIndices[2] >= source->mNumVertices)
            {
                return false;
            }
            triangles.push_back({ { base + face.mIndices[0], base + face.mIndices[1], base + face.mIndices[2] },
                                  material });
        }
    }

    // Refill missing normals from adjacent faces. Isolated vertices use the default up vector.
    if (saw_zero_normal)
    {
        std::vector<vec3> accumulated(vertices.size());
        for (const Triangle &triangle : triangles)
        {
            const vec3 face = cross(
                vertices[triangle.v[1]].pos - vertices[triangle.v[0]].pos,
                vertices[triangle.v[2]].pos - vertices[triangle.v[0]].pos
            );
            accumulated[triangle.v[0]] += face;
            accumulated[triangle.v[1]] += face;
            accumulated[triangle.v[2]] += face;
        }
        for (size_t i = 0; i < vertices.size(); i++)
        {
            if (vertices[i].normal.length_sq() > 0.0f)
            {
                continue;
            }
            vertices[i].normal = accumulated[i].length_sq() > 0.0f ? normalize(accumulated[i]) : vec3(0.0f, 1.0f, 0.0f);
        }
    }

    // Remap sources Assimp leaves Z-up. Terrain and BSP faces also need winding reversal.
    const bool ogex_z_up = extension == ".ogex" && ogex_declared_up_axis(path) != 'y';
    const bool z_up = extension == ".blend" || extension == ".smd" || extension == ".hmp" || extension == ".ter" ||
                      extension == ".bsp" || extension == ".pk3" || extension == ".3d" || extension == ".cob" ||
                      extension == ".scn" || extension == ".amf" || extension == ".3mf" || extension == ".ac" ||
                      extension == ".acc" || extension == ".ac3d" || ogex_z_up;
    if (z_up)
    {
        for (Vertex &vertex : vertices)
        {
            const vec3 pos = vertex.pos;
            const vec3 normal = vertex.normal;
            vertex.pos = { pos.x, pos.z, -pos.y };
            vertex.normal = { normal.x, normal.z, -normal.y };
        }
    }
    if (extension == ".ter" || extension == ".hmp" || extension == ".bsp" || extension == ".pk3")
    {
        for (Triangle &triangle : triangles)
        {
            std::swap(triangle.v[1], triangle.v[2]);
        }
    }
    if (extension == ".ter")
    {
        // Terragen normals were generated before the winding reversal.
        for (Vertex &vertex : vertices)
        {
            vertex.normal = { -vertex.normal.x, -vertex.normal.y, -vertex.normal.z };
        }
    }
    if (extension == ".hmp" || extension == ".bsp" || extension == ".pk3")
    {
        // HMP and BSP texture coordinates are top-down.
        for (Vertex &vertex : vertices)
        {
            vertex.uv.y = 1.0f - vertex.uv.y;
        }
    }

    if (vertices.empty() || triangles.empty())
    {
        return false;
    }
    snapshot.commit();
    return true;
}
