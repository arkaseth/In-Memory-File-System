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
        }
        else
        {
            curr.push_back(c);
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

class FileSystem
{
private:
    std::shared_ptr<DirectoryNode> root;

    // Resolve path and return pair(parentNode, targetNodeName)
    std::pair<std::shared_ptr<DirectoryNode>, std::string> resolveParent(const std::string &path)
    {
        if (path.empty() || path[0] != '/')
            throw std::runtime_error("Path must not be empty and should start with \'/\'");
        auto parts = splitPath(path);
        if (parts.empty())
            throw std::runtime_error("Invalid root parent");

        std::shared_ptr<DirectoryNode> curr = root;
        for (size_t i = 0; i < parts.size() - 1; i++)
        {
            const std::string &p = parts[i];
            auto child = curr->getChild(p);
            if (!child)
                throw std::runtime_error("Path " + p + " not found");
            if (child->type != NodeType::Directory)
            {
                throw std::runtime_error(p + " is not a directory");
            }
            curr = std::static_pointer_cast<DirectoryNode>(child);
        }
        std::string target = parts.back();
        return std::make_pair(curr, target);
    }

    // Traverse the whole path and return node pointer
    std::shared_ptr<INode> traverseNode(const std::string &path)
    {
        if (path == "/")
            return root;
        if (path.empty() || path[0] != '/')
            throw std::runtime_error("Path must not be empty and should start with \'/\'");
        auto parts = splitPath(path);

        std::shared_ptr<INode> curr = root;
        for (auto &p : parts)
        {
            if (curr->type != NodeType::Directory)
            {
                throw std::runtime_error("Path traversed into file instead of directory");
            }

            auto dir = std::static_pointer_cast<DirectoryNode>(curr);
            auto child = dir->getChild(p);
            if (!child)
                throw std::runtime_error("Path " + p + " not found");
            curr = child;
        }
        return curr;
    }

    std::shared_ptr<INode> deepCopyNode(const std::shared_ptr<INode> &src)
    {
        if (src->type == NodeType::File)
        {
            // file cloneShallow copies data as well
            return src->cloneShallow();
        }
        else
        {
            auto srcDir = std::static_pointer_cast<DirectoryNode>(src);
            auto newDir = std::make_shared<DirectoryNode>(srcDir->name);
            newDir->perms = srcDir->perms;
            newDir->created = srcDir->created;
            newDir->modified = srcDir->modified;
            for (const auto &p : srcDir->children)
            {
                auto childCopy = deepCopyNode(p.second);
                childCopy->name = p.first; // ensure copied child name matches key
                newDir->addChild(p.first, childCopy);
            }
            return newDir;
        }
    }

public:
    FileSystem()
    {
        root = std::make_shared<DirectoryNode>("/");
        root->name = "/";
    }

    void mkdir(const std::string &path)
    {
        auto [parent, name] = resolveParent(path);
        if (parent->hasChild(name))
            throw std::runtime_error(name + " already exists");
        auto dir = std::make_shared<DirectoryNode>(name);
        parent->addChild(name, dir);
    }

    void touch(const std::string &path)
    {
        auto [parent, name] = resolveParent(path);
        if (parent->hasChild(name))
            throw std::runtime_error(name + " already exists");
        auto file = std::make_shared<FileNode>(name);
        parent->addChild(name, file);
    }

    void write(const std::string &path, const std::string &content)
    {
        try
        {
            auto node = traverseNode(path);
            if (node->type != NodeType::File)
                throw std::runtime_error("Can't write to directory " + path);
            auto file = std::static_pointer_cast<FileNode>(node);
            file->data.assign(content.begin(), content.end());
            file->modified = time(nullptr);
        }
        catch (const std::runtime_error &e)
        {
            // create file if path not found
            auto [parent, name] = resolveParent(path);
            auto file = std::make_shared<FileNode>(name);
            file->data.assign(content.begin(), content.end());
            parent->addChild(name, file);
        }
    }

    void append(const std::string &path, const std::string &content)
    {
        try
        {
            auto node = traverseNode(path);
            if (node->type != NodeType::File)
                throw std::runtime_error("Can't append to directory " + path);
            auto file = std::static_pointer_cast<FileNode>(node);
            file->data.insert(file->data.end(), content.begin(), content.end());
            file->modified = time(nullptr);
        }
        catch (...)
        {
            touch(path);
            append(path, content);
        }
    }

    std::string read(const std::string &path)
    {
        auto node = traverseNode(path);
        if (node->type != NodeType::File)
            throw std::runtime_error(path + " is a directory");
        return std::static_pointer_cast<FileNode>(node)->readAll();
    }

    std::vector<std::string> ls(const std::string &path)
    {
        auto node = traverseNode(path);
        if (node->type == NodeType::File)
            return {node->name};
        auto dir = std::static_pointer_cast<DirectoryNode>(node);
        return dir->listNames();
    }

    void rm(const std::string &path, bool recursive = false)
    {
        if (path == "/")
            throw std::runtime_error("Can't remove root");
        auto [parent, name] = resolveParent(path);
        auto node = parent->getChild(name);
        if (!node)
            throw std::runtime_error(name + " not found");
        if (node->type == NodeType::Directory)
        {
            auto dir = std::static_pointer_cast<DirectoryNode>(node);
            if (!dir->children.empty() && !recursive)
                throw std::runtime_error("Directory not empty");
        }
        parent->removeChild(name);
    }

    void mv(const std::string &src, const std::string &dest)
    {
        if (src == "/")
            throw std::runtime_error("Cannot move root");
        auto [srcParent, srcName] = resolveParent(src);
        auto node = srcParent->getChild(srcName);
        if (!node)
            throw std::runtime_error("Src not found");

        try
        {
            auto destNode = traverseNode(dest);
            if (destNode->type == NodeType::Directory)
            {
                auto destDir = std::static_pointer_cast<DirectoryNode>(destNode);
                if (destDir->hasChild(srcName))
                    throw std::runtime_error("Target with same name exists in destination");
                srcParent->removeChild(srcName);
                destDir->addChild(srcName, node);
                node->name = srcName;
                return;
            }
            else
            {
                // dest is file -> replace file
                auto [destParent, destName] = resolveParent(dest);
                destParent->removeChild(destName);
                srcParent->removeChild(srcName);
                node->name = destName;
                destParent->addChild(destName, node);
                return;
            }
        }
        catch (const std::runtime_error &e)
        {
            // dest does not exist
            auto [destParent, destName] = resolveParent(dest);
            if (destParent->hasChild(destName))
                throw std::runtime_error("Destination exists");
            srcParent->removeChild(srcName);
            node->name = destName;
            destParent->addChild(destName, node);
            return;
        }
    }

    void cp(const std::string &src, const std::string &dest)
    {
        auto node = traverseNode(src);
        try
        {
            auto destNode = traverseNode(dest);
            if (destNode->type == NodeType::Directory)
            {
                auto destDir = std::static_pointer_cast<DirectoryNode>(destNode);
                if (destDir->hasChild(node->name))
                    throw std::runtime_error("Target with same name exists in destination");
                auto copyNode = deepCopyNode(node);
                destDir->addChild(copyNode->name, copyNode);
                copyNode->name = node->name;
                return;
            }
            else
            {
                throw std::runtime_error("Destination exists and is not a directory");
            }
        }
        catch (...)
        {
            // dest does not exist
            auto [destParent, destName] = resolveParent(dest);
            if (destParent->hasChild(destName))
                throw std::runtime_error("Destination exists");
            auto copyNode = deepCopyNode(node);
            copyNode->name = destName;
            destParent->addChild(copyNode->name, copyNode);
            return;
        }
    }

    void printTree(const std::string &path = "/", int depth = 0)
    {
        auto node = traverseNode(path);
        std::string indent(depth * 2, ' ');
        if (node->type == NodeType::File)
        {
            std::cout << indent << "- " << node->name << " (file, size=" << std::static_pointer_cast<FileNode>(node)->size() << ")\n";
        }
        else
        {
            auto dir = std::static_pointer_cast<DirectoryNode>(node);
            std::cout << indent << "+ " << dir->name << " (dir)\n";
            for (auto &p : dir->children)
            {
                printTree((path == "/" ? "" : path) + "/" + p.first, depth + 1);
            }
        }
    }
};

/* -------------------- Main -------------------- */

int main()
{
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    FileSystem fs;

    fs.mkdir("/home");
    fs.mkdir("/home/arka");
    fs.mkdir("/tmp");

    fs.touch("/home/arka/readme.txt");
    fs.write("/home/arka/readme.txt", "Hello World!\n");
    fs.append("/home/arka/readme.txt", "Hope everyone is well!\n");

    fs.write("/tmp/tmp.txt", "Temp note\n");

    std::cout << "ls /home: ";
    auto list = fs.ls("/home");
    for (auto &s : list)
        std::cout << s << " ";
    std::cout << "\n";

    std::cout << "read /home/arka/readme.txt:\n"
              << fs.read("/home/arka/readme.txt") << "\n";

    fs.cp("/home/arka/readme.txt", "/tmp/readme_copy.txt");
    std::cout << "read /tmp/readme_copy.txt:\n"
              << fs.read("/tmp/readme_copy.txt") << "\n";

    fs.mv("/tmp/readme_copy.txt", "/tmp/readme_moved.txt");

    fs.mkdir("/backup");
    fs.cp("/home", "/backup/home_backup"); // deep copy of subtree
    std::cout << "\nFilesystem tree:\n";
    fs.printTree("/");

    fs.rm("/tmp/tmp.txt");
    std::cout << "\nAfter rm /tmp/tmp.txt, /tmp contains: ";
    for (auto &s : fs.ls("/tmp"))
        std::cout << s << " ";
    std::cout << "\n";

    return 0;
}