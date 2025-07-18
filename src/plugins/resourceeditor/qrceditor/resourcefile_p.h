// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <coreplugin/idocument.h>

#include <utils/textfileformat.h>

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QIcon>
#include <QMap>
#include <QString>
#include <QStringList>

namespace ResourceEditor::Internal {

class File;
struct Prefix;

/*!
    \class Node

    Forms the base class for nodes in a \l ResourceFile tree.
*/
class Node
{
protected:
    Node(File *file, Prefix *prefix) : m_file(file), m_prefix(prefix)
    {
        Q_ASSERT(m_prefix);
    }
public:
    File *file() const { return m_file; }
    Prefix *prefix() const { return m_prefix; }
private:
    File *m_file;
    Prefix *m_prefix;
};

/*!
    \class File

    Represents a file node in a \l ResourceFile tree.
*/
class File : public Node
{
public:
    File(Prefix *prefix, const Utils::FilePath &_name, const QString &_alias = QString());
    void checkExistence();
    bool exists();
    void setExists(bool exists);

    bool operator < (const File &other) const { return name < other.name; }
    bool operator == (const File &other) const { return name == other.name; }
    bool operator != (const File &other) const { return name != other.name; }

    Utils::FilePath name;
    QString alias;
    QIcon icon;

    // not used, only loaded and saved
    QString compress;
    QString compressAlgo;
    QString threshold;

private:
    bool m_checked;
    bool m_exists;
};

using FileList = QList<File *>;

/*!
    \class Prefix

    Represents a prefix node in a \l ResourceFile tree.
*/
struct Prefix : public Node
{
    Prefix(const QString &_name = QString(), const QString &_lang = QString(), const FileList &_file_list = FileList())
        : Node(nullptr, this), name(_name), lang(_lang), file_list(_file_list) {}
    ~Prefix()
    {
        qDeleteAll(file_list);
        file_list.clear();
    }
    bool operator == (const Prefix &other) const { return (name == other.name) && (lang == other.lang); }
    QString name;
    QString lang;
    FileList file_list;
};
using PrefixList = QList<Prefix *>;

/*!
    \class ResourceFile

    Represents the structure of a Qt Resource File (.qrc) file.
*/
class ResourceFile
{
public:
    ResourceFile(const Utils::FilePath &filePath = {}, const QString &contents = {});
    ~ResourceFile();

    void setFilePath(const Utils::FilePath &filePath) { m_filePath = filePath; }
    Utils::FilePath filePath() const { return m_filePath; }

    Utils::Result<> load();
    bool save();
    QString contents() const;
    QString errorMessage() const { return m_error_message; }
    void refresh();

    int prefixCount() const;
    QString prefix(int idx) const;
    QString lang(int idx) const;

    int fileCount(int prefix_idx) const;

    Utils::FilePath file(int prefix_idx, int file_idx) const;
    QString alias(int prefix_idx, int file_idx) const;

    int addFile(int prefix_idx, const QString &file, int file_idx = -1);
    int addPrefix(const QString &prefix, const QString &lang, int prefix_idx = -1);

    void removePrefix(int prefix_idx);
    void removeFile(int prefix_idx, int file_idx);

    bool replacePrefix(int prefix_idx, const QString &prefix);
    bool replaceLang(int prefix_idx, const QString &lang);
    bool replacePrefixAndLang(int prefix_idx, const QString &prefix, const QString &lang);
    void replaceAlias(int prefix_idx, int file_idx, const QString &alias);

    bool renameFile(const Utils::FilePath &fileName, const Utils::FilePath &newFileName);

    void replaceFile(int pref_idx, int file_idx, const Utils::FilePath &file);
    int indexOfPrefix(const QString &prefix, const QString &lang) const;
    int indexOfFile(int pref_idx, const QString &file) const;

    bool contains(const QString &prefix, const QString &lang, const QString &file = QString()) const;
    bool contains(int pref_idx, const QString &file) const;

    QString relativePath(const Utils::FilePath &abs_path) const;
    Utils::FilePath absolutePath(const QString &rel_path) const;

    void orderList();

    static QString fixPrefix(const QString &prefix);

private:
    PrefixList m_prefix_list;
    Utils::FilePath m_filePath;
    QString m_contents;
    QString m_error_message;
    Utils::TextFileFormat m_textFileFormat;

public:
    void * prefixPointer(int prefixIndex) const;
    void * filePointer(int prefixIndex, int fileIndex) const;
    int prefixPointerIndex(const Prefix *prefix) const;

private:
    void clearPrefixList();
    int indexOfPrefix(const QString &prefix, const QString &lang, int skip) const;
};

/*!
    \class ResourceModel

    Wraps a \l ResourceFile as a single-column tree model.
*/
class ResourceModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    ResourceModel();

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    bool hasChildren(const QModelIndex &parent) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void refresh();

    QString errorMessage() const;

    QList<QModelIndex> nonExistingFiles() const;

protected:
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

public:
    Utils::FilePath filePath() const { return m_resource_file.filePath(); }
    void setFilePath(const Utils::FilePath &filePath) { m_resource_file.setFilePath(filePath); }
    void getItem(const QModelIndex &index, QString &prefix, QString &file) const;

    QString lang(const QModelIndex &index) const;
    QString alias(const QModelIndex &index) const;
    Utils::FilePath file(const QModelIndex &index) const;

    virtual QModelIndex addNewPrefix();
    virtual QModelIndex addFiles(const QModelIndex &idx, const QStringList &file_list);
    QStringList existingFilesSubtracted(int prefixIndex, const QStringList &fileNames) const;
    void addFiles(int prefixIndex, const QStringList &fileNames, int cursorFile, int &firstFile, int &lastFile);
    void insertPrefix(int prefixIndex, const QString &prefix, const QString &lang);
    void insertFile(
        int prefixIndex, int fileIndex, const QString &fileName, const QString &alias);
    virtual void changePrefix(const QModelIndex &idx, const QString &prefix);
    virtual void changeLang(const QModelIndex &idx, const QString &lang);
    virtual void changeAlias(const QModelIndex &idx, const QString &alias);
    virtual QModelIndex deleteItem(const QModelIndex &idx);
    QModelIndex getIndex(const QString &prefix, const QString &lang, const QString &file);
    QModelIndex prefixIndex(const QModelIndex &sel_idx) const;

    QString relativePath(const Utils::FilePath &path) const
        { return m_resource_file.relativePath(path); }

private:
    QString lastResourceOpenDirectory() const;
    bool renameFile(const Utils::FilePath &filePath, const Utils::FilePath &newFileName);

public:
    virtual Utils::Result<> reload();
    virtual bool save();
    QString contents() const { return m_resource_file.contents(); }

    // QString errorMessage() const { return m_resource_file.errorMessage(); }

    bool dirty() const { return m_dirty; }
    void setDirty(bool b);

    void orderList();

private:
    QMimeData *mimeData (const QModelIndexList & indexes) const override;

    static bool hasIconFileExtension(const QString &path);
    static QString resourcePath(const QString &prefix, const QString &file);

signals:
    void dirtyChanged(bool b);
    void contentsChanged();

private:
    ResourceFile m_resource_file;
    bool m_dirty = false;
    QString m_lastResourceDir;
    QIcon m_prefixIcon;
};

/*!
    \class EntryBackup

    Holds the backup of a tree node including children.
*/
class EntryBackup
{
protected:
    ResourceModel *m_model;
    int m_prefixIndex;
    QString m_name;

    EntryBackup(ResourceModel &model, int prefixIndex, const QString &name)
            : m_model(&model), m_prefixIndex(prefixIndex), m_name(name) { }

public:
    virtual void restore() const = 0;
    virtual ~EntryBackup() = default;
};

class RelativeResourceModel final : public ResourceModel
{
public:
    RelativeResourceModel();

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid())
            return QVariant();
/*
        void const * const internalPointer = index.internalPointer();

        if ((role == Qt::DisplayRole) && (internalPointer != NULL))
            return ResourceModel::data(index, Qt::ToolTipRole);
*/
        return ResourceModel::data(index, role);
    }

    void setResourceDragEnabled(bool e) { m_resourceDragEnabled = e; }
    bool resourceDragEnabled() const { return m_resourceDragEnabled; }

    Qt::ItemFlags flags(const QModelIndex &index) const override;

    EntryBackup * removeEntry(const QModelIndex &index);

private:
    bool m_resourceDragEnabled = false;
};

} // ResourceEditor::Internal
