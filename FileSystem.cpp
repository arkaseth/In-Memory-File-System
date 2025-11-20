#include <iostream>
#include <vector>
#include <memory>
#include <unordered_map>
#include <algorithm>

/* ----------------------- Basic Helpers and Types ----------------------- */
enum class NodeType
{
    Directory,
    File
};

struct Permissions
{
    // rws bits per owner/group.others represented by ints 0-7
    int owner = 6;  // rw- by default (4+2)
    int group = 4;  // r--
    int others = 4; // r--
};

static std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> parts;
    std::string curr;
    for (size_t i = 0; i < path.size(); i++)
    {
        char c = path[i];
        if (c == '/')
        {
            if (!curr.empty())
            {
                parts.push_back(curr);
                curr.clear();
            }
            else
            {
                curr.push_back(c);
            }
        }
    }
    if (!curr.empty())
        parts.push_back(curr);
    return parts;
}

/* -------------------- INode, DirectoryNode, FileNode -------------------- */

struct INode : std::enable_shared_from_this<INode>
{
    std::string name;
    NodeType type;
    Permissions perms;
    time_t created;
    time_t modified;

    INode(std::string _name, NodeType t) : name(move(_name)), type(t)
    {
        created = modified = time(nullptr);
    }

    virtual ~INode() = default;

    virtual std::shared_ptr<INode> cloneShallow() const = 0; // copy metadata, not data
};

struct DirectoryNode : INode
{
    std::unordered_map<std::string, std::shared_ptr<INode>> children;

    DirectoryNode(const std::string &_name) : INode(_name, NodeType::Directory) {}

    std::shared_ptr<INode> cloneShallow() const override
    {
        // children not copied here, that is done in deep copy only
        auto d = std::make_shared<DirectoryNode>(name);
        d->perms = perms;
        d->created = created;
        d->modified = modified;
        return d;
    }

    bool hasChild(const std::string &n) const
    {
        return children.find(n) != children.end();
    }

    std::shared_ptr<INode> getChild(const std::string &n) const
    {
        auto it = children.find(n);
        if (it == children.end())
            return nullptr;
        return it->second;
    }

    void addChild(const std::string &n, std::shared_ptr<INode> node)
    {
        children[n] = node;
        modified = time(nullptr);
    }

    void removeChild(const std::string &n)
    {
        children.erase(n);
        modified = time(nullptr);
    }

    std::vector<std::string> listNames() const
    {
        std::vector<std::string> out;
        out.reserve(children.size());
        for (auto &p : children)
            out.push_back(p.first);
        sort(out.begin(), out.end());
        return out;
    }
};

struct FileNode : INode
{
    std::vector<char> data;

    FileNode(const std::string &_name) : INode(_name, NodeType::File) {}

    size_t size() const { return data.size(); }

    std::shared_ptr<INode> cloneShallow() const override
    {
        auto f = std::make_shared<FileNode>(name);
        f->perms = perms;
        f->created = created;
        f->modified = modified;
        f->data = data; // deep copy
        return f;
    }

    void write(const std::string &s, size_t offset = 0)
    {
        if (offset < data.size())
            data.resize(offset);
        if (offset + s.size() > data.size())
            data.resize(offset + s.size());
        copy(s.begin(), s.end(), data.begin() + offset);
        modified = time(nullptr);
    }

    std::string readAll() const
    {
        return std::string(data.begin(), data.end());
    }
};

/* -------------------- FileSystem Class -------------------- */