// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include <set>
#include "tree_grid.h"
#include <wx/settings.h>
#include <wx/menu.h>
#include <zen/i18n.h>
#include <zen/utf.h>
#include <zen/stl_tools.h>
#include <zen/format_unit.h>
#include <wx+/rtl.h>
#include <wx+/dc.h>
#include <wx+/context_menu.h>
#include <wx+/image_resources.h>
#include <wx+/image_tools.h>
#include "../icon_buffer.h"

using namespace zen;
using namespace fff;


namespace
{
//let's NOT create wxWidgets objects statically:
const int PERCENTAGE_BAR_WIDTH_DIP = 60;
const int TREE_GRID_GAP_SIZE_DIP   = 2;

inline wxColor getColorPercentBorder    () { return { 198, 198, 198 }; }
inline wxColor getColorPercentBackground() { return { 0xf8, 0xf8, 0xf8 }; }
}


TreeView::TreeView(FolderComparison& folderCmp, const SortInfo& si) : folderCmp_(folderCmp), currentSort_(si)
{
    //remove truly empty folder pairs as early as this: we want to distinguish single/multiple folder pair cases by looking at "folderCmp"
    std::erase_if(folderCmp_, [](const std::shared_ptr<BaseFolderPair>& baseObj)
    {
        return AFS::isNullPath(baseObj->getAbstractPath< LEFT_SIDE>()) &&
               AFS::isNullPath(baseObj->getAbstractPath<RIGHT_SIDE>());
    });
}


inline
void TreeView::compressNode(Container& cont) //remove single-element sub-trees -> gain clarity + usability (call *after* inclusion check!!!)
{
    if (cont.subDirs.empty()) //single files node
        cont.firstFileId = nullptr;

#if 0 //let's not go overboard: empty folders should not be condensed => used for file exclusion filter; user expects to see them
    if (cont.firstFileId == nullptr && //single dir node...
        cont.subDirs.size() == 1   && //
        cont.subDirs[0].firstFileId == nullptr && //...that is empty
        cont.subDirs[0].subDirs.empty())          //
        cont.subDirs.clear();
#endif
}


template <class Function> //(const FileSystemObject&) -> bool
void TreeView::extractVisibleSubtree(ContainerObject& hierObj,  //in
                                     TreeView::Container& cont, //out
                                     Function pred)
{
    auto getBytes = [](const FilePair& file) //MSVC screws up miserably if we put this lambda into std::for_each
    {
        ////give accumulated bytes the semantics of a sync preview!
        //if (file.isActive())
        //    switch (file.getSyncDir())
        //    {
        //        case SyncDirection::left:
        //            return file.getFileSize<RIGHT_SIDE>();
        //        case SyncDirection::right:
        //            return file.getFileSize<LEFT_SIDE>();
        //        case SyncDirection::none:
        //            break;
        //    }

        //prefer file-browser semantics over sync preview (=> always show useful numbers, even for SyncDirection::none)
        //discussion: https://freefilesync.org/forum/viewtopic.php?t=1595
        return std::max(file.getFileSize<LEFT_SIDE>(), file.getFileSize<RIGHT_SIDE>());
    };

    cont.firstFileId = nullptr;
    for (FilePair& file : hierObj.refSubFiles())
        if (pred(file))
        {
            cont.bytesNet += getBytes(file);
            ++cont.itemCountNet;

            if (!cont.firstFileId)
                cont.firstFileId = file.getId();
        }

    for (SymlinkPair& symlink : hierObj.refSubLinks())
        if (pred(symlink))
        {
            ++cont.itemCountNet;

            if (!cont.firstFileId)
                cont.firstFileId = symlink.getId();
        }

    cont.bytesGross     += cont.bytesNet;
    cont.itemCountGross += cont.itemCountNet;

    cont.subDirs.reserve(hierObj.refSubFolders().size()); //avoid expensive reallocations!

    for (FolderPair& folder : hierObj.refSubFolders())
    {
        const bool included = pred(folder);

        cont.subDirs.emplace_back(); //
        auto& subDirCont = cont.subDirs.back();
        TreeView::extractVisibleSubtree(folder, subDirCont, pred);
        if (included)
            ++subDirCont.itemCountGross;

        cont.bytesGross     += subDirCont.bytesGross;
        cont.itemCountGross += subDirCont.itemCountGross;

        if (!included && !subDirCont.firstFileId && subDirCont.subDirs.empty())
            cont.subDirs.pop_back();
        else
        {
            subDirCont.objId = folder.getId();
            compressNode(subDirCont);
        }
    }
}


namespace
{
//generate nice percentage numbers which precisely sum up to 100
void calcPercentage(std::vector<std::pair<uint64_t, int*>>& workList)
{
    const uint64_t total = std::accumulate(workList.begin(), workList.end(), uint64_t(),
    [](uint64_t sum, const std::pair<uint64_t, int*>& pair) { return sum + pair.first; });

    if (total == 0U) //this case doesn't work with the error minimizing algorithm below
    {
        for (auto& [bytes, percent] : workList)
            *percent = 0;
        return;
    }

    int remainingPercent = 100;
    for (auto& [bytes, percent] : workList)
    {
        *percent = static_cast<int>(bytes * 100U / total); //round down
        remainingPercent -= *percent;
    }
    assert(remainingPercent >= 0);
    assert(remainingPercent < std::ssize(workList));

    //distribute remaining percent so that overall error is minimized as much as possible:
    remainingPercent = std::min(remainingPercent, static_cast<int>(workList.size()));
    if (remainingPercent > 0)
    {
        std::nth_element(workList.begin(), workList.begin() + remainingPercent - 1, workList.end(),
                         [total](const std::pair<uint64_t, int*>& lhs, const std::pair<uint64_t, int*>& rhs)
        {
            return lhs.first * 100U % total > rhs.first * 100U % total;
        });

        std::for_each(workList.begin(), workList.begin() + remainingPercent, [&](std::pair<uint64_t, int*>& pair) { ++*pair.second; });
    }
}
}


template <bool ascending>
struct TreeView::LessShortName
{
    bool operator()(const TreeLine& lhs, const TreeLine& rhs) const
    {
        //files last (irrespective of sort direction)
        if (lhs.type == NodeType::files)
            return false;
        else if (rhs.type == NodeType::files)
            return true;

        if (lhs.type != rhs.type)       //
            return lhs.type < rhs.type; //shouldn't happen! root nodes not mixed with files or directories

        switch (lhs.type)
        {
            case NodeType::root:
                return makeSortDirection(LessNaturalSort() /*even on Linux*/,
                                         std::bool_constant<ascending>())(utfTo<Zstring>(static_cast<const RootNodeImpl*>(lhs.node)->displayName),
                                                                          utfTo<Zstring>(static_cast<const RootNodeImpl*>(rhs.node)->displayName));

            case NodeType::folder:
            {
                const auto* folderL = dynamic_cast<const FolderPair*>(FileSystemObject::retrieve(static_cast<const DirNodeImpl*>(lhs.node)->objId));
                const auto* folderR = dynamic_cast<const FolderPair*>(FileSystemObject::retrieve(static_cast<const DirNodeImpl*>(rhs.node)->objId));

                if (!folderL)  //might be pathologic, but it's covered
                    return false;
                else if (!folderR)
                    return true;

                return makeSortDirection(LessNaturalSort(), std::bool_constant<ascending>())(folderL->getItemNameAny(), folderR->getItemNameAny());
            }

            case NodeType::files:
                break;
        }
        assert(false);
        return false; //:= all equal
    }
};


template <bool ascending>
void TreeView::sortSingleLevel(std::vector<TreeLine>& items, ColumnTypeTree columnType)
{
    auto getBytes = [](const TreeLine& line) -> uint64_t
    {
        switch (line.type)
        {
            case NodeType::root:
            case NodeType::folder:
                return line.node->bytesGross;
            case NodeType::files:
                return line.node->bytesNet;
        }
        assert(false);
        return 0U;
    };

    auto getCount = [](const TreeLine& line) -> int
    {
        switch (line.type)
        {
            case NodeType::root:
            case NodeType::folder:
                return line.node->itemCountGross;

            case NodeType::files:
                return line.node->itemCountNet;
        }
        assert(false);
        return 0;
    };

    const auto lessBytes = [&](const TreeLine& lhs, const TreeLine& rhs) { return getBytes(lhs) < getBytes(rhs); };
    const auto lessCount = [&](const TreeLine& lhs, const TreeLine& rhs) { return getCount(lhs) < getCount(rhs); };

    switch (columnType)
    {
        case ColumnTypeTree::folder:
            std::sort(items.begin(), items.end(), LessShortName<ascending>());
            break;
        case ColumnTypeTree::itemCount:
            std::sort(items.begin(), items.end(), makeSortDirection(lessCount, std::bool_constant<ascending>()));
            break;
        case ColumnTypeTree::bytes:
            std::sort(items.begin(), items.end(), makeSortDirection(lessBytes, std::bool_constant<ascending>()));
            break;
    }
}


void TreeView::getChildren(const Container& cont, unsigned int level, std::vector<TreeLine>& output)
{
    output.clear();
    output.reserve(cont.subDirs.size() + 1); //keep pointers in "workList" valid
    std::vector<std::pair<uint64_t, int*>> workList;

    for (const DirNodeImpl& subDir : cont.subDirs)
    {
        output.push_back({ level, 0, &subDir, NodeType::folder});
        workList.emplace_back(subDir.bytesGross, &output.back().percent);
    }

    if (cont.firstFileId)
    {
        output.push_back({ level, 0, &cont, NodeType::files });
        workList.emplace_back(cont.bytesNet, &output.back().percent);
    }
    calcPercentage(workList);

    if (currentSort_.ascending)
        sortSingleLevel<true>(output, currentSort_.sortCol);
    else
        sortSingleLevel<false>(output, currentSort_.sortCol);
}


void TreeView::applySubView(std::vector<RootNodeImpl>&& newView)
{
    //preserve current node expansion status
    auto getHierAlias = [](const TreeView::TreeLine& tl) -> const ContainerObject*
    {
        switch (tl.type)
        {
            case NodeType::root:
                return static_cast<const RootNodeImpl*>(tl.node)->baseFolder.get();

            case NodeType::folder:
                if (auto folder = dynamic_cast<const FolderPair*>(FileSystemObject::retrieve(static_cast<const DirNodeImpl*>(tl.node)->objId)))
                    return folder;
                break;

            case NodeType::files:
                break; //none!!!
        }
        return nullptr;
    };

    std::unordered_set<const ContainerObject*> expandedNodes;
    if (!flatTree_.empty())
    {
        auto it = flatTree_.begin();
        for (auto iterNext = flatTree_.begin() + 1; iterNext != flatTree_.end(); ++iterNext, ++it)
            if (it->level < iterNext->level)
                if (auto hierObj = getHierAlias(*it))
                    expandedNodes.insert(hierObj);
    }

    //update view on full data
    folderCmpView_.swap(newView); //newView may be an alias for folderCmpView! see sorting!

    //set default flat tree
    flatTree_.clear();

    if (folderCmp_.size() == 1) //single folder pair case (empty pairs were already removed!) do NOT use folderCmpView for this check!
    {
        if (!folderCmpView_.empty()) //possibly empty!
            getChildren(folderCmpView_[0], 0, flatTree_); //do not show root
    }
    else
    {
        //following is almost identical with TreeView::getChildren(): however we *cannot* reuse code here;
        //this were only possible if we replaced "std::vector<RootNodeImpl>" with "Container"!

        flatTree_.reserve(folderCmpView_.size()); //keep pointers in "workList" valid
        std::vector<std::pair<uint64_t, int*>> workList;

        for (const RootNodeImpl& root : folderCmpView_)
        {
            flatTree_.push_back({ 0, 0, &root, NodeType::root });
            workList.emplace_back(root.bytesGross, &flatTree_.back().percent);
        }

        calcPercentage(workList);

        if (currentSort_.ascending)
            sortSingleLevel<true>(flatTree_, currentSort_.sortCol);
        else
            sortSingleLevel<false>(flatTree_, currentSort_.sortCol);
    }

    //restore node expansion status
    for (size_t row = 0; row < flatTree_.size(); ++row) //flatTree size changes during loop!
    {
        const TreeLine& line = flatTree_[row];

        if (auto hierObj = getHierAlias(line))
            if (expandedNodes.contains(hierObj))
            {
                std::vector<TreeLine> newLines;
                getChildren(*line.node, line.level + 1, newLines);

                flatTree_.insert(flatTree_.begin() + row + 1, newLines.begin(), newLines.end());
            }
    }
}


template <class Predicate>
void TreeView::updateView(Predicate pred)
{
    //update view on full data
    std::vector<RootNodeImpl> newView;
    newView.reserve(folderCmp_.size()); //avoid expensive reallocations!

    for (const std::shared_ptr<BaseFolderPair>& baseObj : folderCmp_)
    {
        newView.emplace_back();
        RootNodeImpl& root = newView.back();
        this->extractVisibleSubtree(*baseObj, root, pred); //"this->" is bogus for a static method, but GCC screws this one up

        //warning: the following lines are almost 1:1 copy from extractVisibleSubtree:
        //however we *cannot* reuse code here; this were only possible if we replaced "std::vector<RootNodeImpl>" with "Container"!
        if (!root.firstFileId && root.subDirs.empty())
            newView.pop_back();
        else
        {
            root.baseFolder = baseObj;
            root.displayName = getShortDisplayNameForFolderPair(baseObj->getAbstractPath< LEFT_SIDE>(),
                                                                baseObj->getAbstractPath<RIGHT_SIDE>());

            this->compressNode(root); //"this->" required by two-pass lookup as enforced by GCC 4.7
        }
    }

    lastViewFilterPred_ = pred;
    applySubView(std::move(newView));
}


void TreeView::setSortDirection(ColumnTypeTree colType, bool ascending) //apply permanently!
{
    currentSort_ = SortInfo{colType, ascending};

    //reapply current view
    applySubView(std::move(folderCmpView_));
}


TreeView::NodeStatus TreeView::getStatus(size_t row) const
{
    if (row < flatTree_.size())
    {
        if (row + 1 < flatTree_.size() && flatTree_[row + 1].level > flatTree_[row].level)
            return TreeView::STATUS_EXPANDED;

        //it's either reduced or empty
        switch (flatTree_[row].type)
        {
            case NodeType::root:
            case NodeType::folder:
                return flatTree_[row].node->firstFileId || !flatTree_[row].node->subDirs.empty() ? TreeView::STATUS_REDUCED : TreeView::STATUS_EMPTY;

            case NodeType::files:
                return TreeView::STATUS_EMPTY;
        }
    }
    return TreeView::STATUS_EMPTY;
}


void TreeView::expandNode(size_t row)
{
    if (getStatus(row) != TreeView::STATUS_REDUCED)
    {
        assert(false);
        return;
    }

    if (row < flatTree_.size())
    {
        std::vector<TreeLine> newLines;

        switch (flatTree_[row].type)
        {
            case NodeType::root:
            case NodeType::folder:
                getChildren(*flatTree_[row].node, flatTree_[row].level + 1, newLines);
                break;
            case NodeType::files:
                break;
        }
        flatTree_.insert(flatTree_.begin() + row + 1, newLines.begin(), newLines.end());
    }
}


void TreeView::reduceNode(size_t row)
{
    if (row < flatTree_.size())
    {
        const unsigned int parentLevel = flatTree_[row].level;

        bool done = false;
        flatTree_.erase(std::remove_if(flatTree_.begin() + row + 1, flatTree_.end(),
                                       [&](const TreeLine& line) -> bool
        {
            if (done)
                return false;
            if (line.level > parentLevel)
                return true;
            else
            {
                done = true;
                return false;
            }
        }), flatTree_.end());
    }
}


ptrdiff_t TreeView::getParent(size_t row) const
{
    if (row < flatTree_.size())
    {
        const auto level = flatTree_[row].level;

        while (row-- > 0)
            if (flatTree_[row].level < level)
                return row;
    }
    return -1;
}


void TreeView::applyFilterByCategory(bool showExcluded,
                                     bool leftOnlyFilesActive,
                                     bool rightOnlyFilesActive,
                                     bool leftNewerFilesActive,
                                     bool rightNewerFilesActive,
                                     bool differentFilesActive,
                                     bool equalFilesActive,
                                     bool conflictFilesActive)
{
    updateView([showExcluded, //make sure the predicate can be stored safely!
                              leftOnlyFilesActive,
                              rightOnlyFilesActive,
                              leftNewerFilesActive,
                              rightNewerFilesActive,
                              differentFilesActive,
                              equalFilesActive,
                              conflictFilesActive](const FileSystemObject& fsObj) -> bool
    {
        if (!fsObj.isActive() && !showExcluded)
            return false;

        switch (fsObj.getCategory())
        {
            case FILE_LEFT_SIDE_ONLY:
                return leftOnlyFilesActive;
            case FILE_RIGHT_SIDE_ONLY:
                return rightOnlyFilesActive;
            case FILE_LEFT_NEWER:
                return leftNewerFilesActive;
            case FILE_RIGHT_NEWER:
                return rightNewerFilesActive;
            case FILE_DIFFERENT_CONTENT:
                return differentFilesActive;
            case FILE_EQUAL:
            case FILE_DIFFERENT_METADATA: //= sub-category of equal
                return equalFilesActive;
            case FILE_CONFLICT:
                return conflictFilesActive;
        }
        assert(false);
        return true;
    });
}


void TreeView::applyFilterByAction(bool showExcluded,
                                   bool syncCreateLeftActive,
                                   bool syncCreateRightActive,
                                   bool syncDeleteLeftActive,
                                   bool syncDeleteRightActive,
                                   bool syncDirOverwLeftActive,
                                   bool syncDirOverwRightActive,
                                   bool syncDirNoneActive,
                                   bool syncEqualActive,
                                   bool conflictFilesActive)
{
    updateView([showExcluded, //make sure the predicate can be stored safely!
                              syncCreateLeftActive,
                              syncCreateRightActive,
                              syncDeleteLeftActive,
                              syncDeleteRightActive,
                              syncDirOverwLeftActive,
                              syncDirOverwRightActive,
                              syncDirNoneActive,
                              syncEqualActive,
                              conflictFilesActive](const FileSystemObject& fsObj) -> bool
    {
        if (!fsObj.isActive() && !showExcluded)
            return false;

        switch (fsObj.getSyncOperation())
        {
            case SO_CREATE_NEW_LEFT:
                return syncCreateLeftActive;
            case SO_CREATE_NEW_RIGHT:
                return syncCreateRightActive;
            case SO_DELETE_LEFT:
                return syncDeleteLeftActive;
            case SO_DELETE_RIGHT:
                return syncDeleteRightActive;
            case SO_OVERWRITE_RIGHT:
            case SO_COPY_METADATA_TO_RIGHT:
            case SO_MOVE_RIGHT_FROM:
            case SO_MOVE_RIGHT_TO:
                return syncDirOverwRightActive;
            case SO_OVERWRITE_LEFT:
            case SO_COPY_METADATA_TO_LEFT:
            case SO_MOVE_LEFT_FROM:
            case SO_MOVE_LEFT_TO:
                return syncDirOverwLeftActive;
            case SO_DO_NOTHING:
                return syncDirNoneActive;
            case SO_EQUAL:
                return syncEqualActive;
            case SO_UNRESOLVED_CONFLICT:
                return conflictFilesActive;
        }
        assert(false);
        return true;
    });
}


std::unique_ptr<TreeView::Node> TreeView::getLine(size_t row) const
{
    if (row < flatTree_.size())
    {
        const auto level  = flatTree_[row].level;
        const int percent = flatTree_[row].percent;

        switch (flatTree_[row].type)
        {
            case NodeType::root:
            {
                const auto& root = *static_cast<const TreeView::RootNodeImpl*>(flatTree_[row].node);
                return std::make_unique<TreeView::RootNode>(percent, root.bytesGross, root.itemCountGross, getStatus(row), *root.baseFolder, root.displayName);
            }
            break;

            case NodeType::folder:
            {
                const auto* dir = static_cast<const TreeView::DirNodeImpl*>(flatTree_[row].node);
                if (auto folder = dynamic_cast<FolderPair*>(FileSystemObject::retrieve(dir->objId)))
                    return std::make_unique<TreeView::DirNode>(percent, dir->bytesGross, dir->itemCountGross, level, getStatus(row), *folder);
            }
            break;

            case NodeType::files:
            {
                const auto* parentDir = flatTree_[row].node;
                if (FileSystemObject* firstFile = FileSystemObject::retrieve(parentDir->firstFileId))
                {
                    std::vector<FileSystemObject*> filesAndLinks;
                    ContainerObject& parent = firstFile->parent();

                    //lazy evaluation: recheck "lastViewFilterPred" again rather than buffer and bloat "lastViewFilterPred"
                    for (FileSystemObject& fsObj : parent.refSubFiles())
                        if (lastViewFilterPred_(fsObj))
                            filesAndLinks.push_back(&fsObj);

                    for (FileSystemObject& fsObj : parent.refSubLinks())
                        if (lastViewFilterPred_(fsObj))
                            filesAndLinks.push_back(&fsObj);

                    return std::make_unique<TreeView::FilesNode>(percent, parentDir->bytesNet, parentDir->itemCountNet, level, filesAndLinks);
                }
            }
            break;
        }
    }
    return nullptr;
}

//##########################################################################################################

namespace
{
wxColor getColorForLevel(size_t level)
{
    switch (level % 12)
    {
        //*INDENT-OFF*
        case  0: return { 0xcc, 0xcc, 0xff };
        case  1: return { 0xcc, 0xff, 0xcc };
        case  2: return { 0xff, 0xff, 0x99 };
        case  3: return { 0xdd, 0xdd, 0xdd };
        case  4: return { 0xff, 0xcc, 0xff };
        case  5: return { 0x99, 0xff, 0xcc };
        case  6: return { 0xcc, 0xcc, 0x99 };
        case  7: return { 0xff, 0xcc, 0xcc };
        case  8: return { 0xcc, 0xff, 0x99 };
        case  9: return { 0xff, 0xff, 0xcc };
        case 10: return { 0xcc, 0xff, 0xff };
        case 11: return { 0xff, 0xcc, 0x99 };
        //*INDENT-ON*
    }
    assert(false);
    return *wxBLACK;
}


class GridDataTree : private wxEvtHandler, public GridData
{
public:
    GridDataTree(Grid& grid) :
        widthNodeIcon_(IconBuffer::getSize(IconBuffer::SIZE_SMALL)),
        widthLevelStep_(widthNodeIcon_),
        widthNodeStatus_(loadImage("node_expanded").GetWidth()),
        rootIcon_(loadImage("root_folder", widthNodeIcon_)),
        grid_(grid)
    {
        grid.getMainWin().Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { onKeyDown(event); });
        grid.Bind(EVENT_GRID_MOUSE_LEFT_DOWN,       [this](GridClickEvent& event) { onMouseLeft      (event); });
        grid.Bind(EVENT_GRID_MOUSE_LEFT_DOUBLE,     [this](GridClickEvent& event) { onMouseLeftDouble(event); });
        grid.Bind(EVENT_GRID_COL_LABEL_MOUSE_RIGHT, [this](GridLabelClickEvent& event) { onGridLabelContext  (event); });
        grid.Bind(EVENT_GRID_COL_LABEL_MOUSE_LEFT,  [this](GridLabelClickEvent& event) { onGridLabelLeftClick(event); });
    }

    void setData(FolderComparison& folderCmp)
    {
        const TreeView::SortInfo sortCfg = treeDataView_.ref().getSortConfig(); //preserve!

        treeDataView_ = makeSharedRef<TreeView>(); //clear old data view first! avoid memory peaks!
        treeDataView_ = makeSharedRef<TreeView>(folderCmp, sortCfg);
    }

    const TreeView& getDataView() const { return treeDataView_.ref(); }
    /**/  TreeView& getDataView()       { return treeDataView_.ref(); }

    void setShowPercentage(bool value) { showPercentBar_ = value; grid_.Refresh(); }
    bool getShowPercentage() const { return showPercentBar_; }

private:
    size_t getRowCount() const override { return getDataView().rowsTotal(); }

    std::wstring getToolTip(size_t row, ColumnType colType) const override
    {
        switch (static_cast<ColumnTypeTree>(colType))
        {
            case ColumnTypeTree::folder:
                if (std::unique_ptr<TreeView::Node> node = getDataView().getLine(row))
                    if (const TreeView::RootNode* root = dynamic_cast<const TreeView::RootNode*>(node.get()))
                    {
                        const std::wstring& dirLeft  = AFS::getDisplayPath(root->baseFolder.getAbstractPath< LEFT_SIDE>());
                        const std::wstring& dirRight = AFS::getDisplayPath(root->baseFolder.getAbstractPath<RIGHT_SIDE>());
                        if (dirLeft.empty())
                            return dirRight;
                        else if (dirRight.empty())
                            return dirLeft;
                        return dirLeft + L' ' + EN_DASH + L'\n' + dirRight;
                    }
                break;

            case ColumnTypeTree::itemCount:
            case ColumnTypeTree::bytes:
                break;
        }
        return std::wstring();
    }

    std::wstring getValue(size_t row, ColumnType colType) const override
    {
        if (std::unique_ptr<TreeView::Node> node = getDataView().getLine(row))
            switch (static_cast<ColumnTypeTree>(colType))
            {
                case ColumnTypeTree::folder:
                    if (const TreeView::RootNode* root = dynamic_cast<const TreeView::RootNode*>(node.get()))
                        return root->displayName;
                    else if (const TreeView::DirNode* dir = dynamic_cast<const TreeView::DirNode*>(node.get()))
                        return utfTo<std::wstring>(dir->folder.getItemNameAny());
                    else if (dynamic_cast<const TreeView::FilesNode*>(node.get()))
                        return _("Files");
                    break;

                case ColumnTypeTree::itemCount:
                    return formatNumber(node->itemCount_);

                case ColumnTypeTree::bytes:
                    return formatFilesizeShort(node->bytes_);
            }

        return std::wstring();
    }

    void renderColumnLabel(wxDC& dc, const wxRect& rect, ColumnType colType, bool enabled, bool highlighted) override
    {
        const auto colTypeTree = static_cast<ColumnTypeTree>(colType);

        const wxRect rectInner = drawColumnLabelBackground(dc, rect, highlighted);
        wxRect rectRemain = rectInner;

        rectRemain.x     += getColumnGapLeft();
        rectRemain.width -= getColumnGapLeft();
        drawColumnLabelText(dc, rectRemain, getColumnLabel(colType), enabled);

        if (const auto [sortCol, ascending] = getDataView().getSortConfig();
            colTypeTree == sortCol)
        {
            const wxImage sortMarker = loadImage(ascending ? "sort_ascending" : "sort_descending");
            drawBitmapRtlNoMirror(dc, enabled ? sortMarker : sortMarker.ConvertToDisabled(), rectInner, wxALIGN_CENTER_HORIZONTAL);
        }
    }


    void renderRowBackgound(wxDC& dc, const wxRect& rect, size_t row, bool enabled, bool selected) override
    {
        if (!enabled || !selected)
            clearArea(dc, rect, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
        else
            GridData::renderRowBackgound(dc, rect, row, true /*enabled*/, true /*selected*/ );
    }


    enum class HoverAreaTree
    {
        node,
    };

    void renderCell(wxDC& dc, const wxRect& rect, size_t row, ColumnType colType, bool enabled, bool selected, HoverArea rowHover) override
    {
        wxDCTextColourChanger textColor(dc);
        if (enabled && selected) //accessibility: always set *both* foreground AND background colors!
            textColor.Set(*wxBLACK);

        //wxRect rectTmp= drawCellBorder(dc, rect);
        wxRect rectTmp = rect;

        //  Partitioning:
        //   ________________________________________________________________________________
        //  | space | gap | percentage bar | 2 x gap | node status | gap |icon | gap | rest |
        //   --------------------------------------------------------------------------------
        // -> synchronize renderCell() <-> getBestSize() <-> getRowMouseHover()

        if (static_cast<ColumnTypeTree>(colType) == ColumnTypeTree::folder)
        {
            if (std::unique_ptr<TreeView::Node> node = getDataView().getLine(row))
            {
                ////clear first section:
                //clearArea(dc, wxRect(rect.GetTopLeft(), wxSize(
                //                         node->level_ * widthLevelStep_ + gridGap_ + //width
                //                         (showPercentBar ? percentageBarWidth_ + 2 * gridGap_ : 0) + //
                //                         widthNodeStatus_ + gridGap_ + widthNodeIcon + gridGap_, //
                //                         rect.height)), wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

                auto drawIcon = [&](wxImage icon, const wxRect& rectIcon, bool drawActive)
                {
                    if (!drawActive)
                        icon = icon.ConvertToGreyscale(1.0 / 3, 1.0 / 3, 1.0 / 3); //treat all channels equally!

                    if (!enabled)
                        icon = icon.ConvertToDisabled();

                    drawBitmapRtlNoMirror(dc, icon, rectIcon, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
                };

                //consume space
                rectTmp.x     += static_cast<int>(node->level_) * widthLevelStep_;
                rectTmp.width -= static_cast<int>(node->level_) * widthLevelStep_;

                rectTmp.x     += gridGap_;
                rectTmp.width -= gridGap_;

                if (rectTmp.width > 0)
                {
                    //percentage bar
                    if (showPercentBar_)
                    {
                        wxRect areaPerc(rectTmp.x, rectTmp.y + fastFromDIP(2), percentageBarWidth_, rectTmp.height - fastFromDIP(4));
                        //clear background
                        drawFilledRectangle(dc, areaPerc, fastFromDIP(1), getColorPercentBorder(), getColorPercentBackground());
                        areaPerc.Deflate(fastFromDIP(1));

                        //inner area
                        wxRect areaPercTmp = areaPerc;
                        areaPercTmp.width = numeric::round(areaPercTmp.width * node->percent_ / 100.0);
                        clearArea(dc, areaPercTmp, getColorForLevel(node->level_));

                        wxDCTextColourChanger textColorPercent(dc, *wxBLACK); //accessibility: always set both foreground AND background colors!
                        drawCellText(dc, areaPerc, numberTo<std::wstring>(node->percent_) + L"%", wxALIGN_CENTER);

                        rectTmp.x     += percentageBarWidth_ + 2 * gridGap_;
                        rectTmp.width -= percentageBarWidth_ + 2 * gridGap_;
                    }
                    if (rectTmp.width > 0)
                    {
                        //node status
                        const bool drawMouseHover = static_cast<HoverAreaTree>(rowHover) == HoverAreaTree::node;
                        switch (node->status_)
                        {
                            case TreeView::STATUS_EXPANDED:
                                drawIcon(loadImage(drawMouseHover ? "node_expanded_hover" : "node_expanded"), rectTmp, true /*drawActive*/);
                                break;
                            case TreeView::STATUS_REDUCED:
                                drawIcon(loadImage(drawMouseHover ? "node_reduced_hover" : "node_reduced"), rectTmp, true /*drawActive*/);
                                break;
                            case TreeView::STATUS_EMPTY:
                                break;
                        }

                        rectTmp.x     += widthNodeStatus_ + gridGap_;
                        rectTmp.width -= widthNodeStatus_ + gridGap_;
                        if (rectTmp.width > 0)
                        {
                            wxImage nodeIcon;
                            bool isActive = true;
                            //icon
                            if (dynamic_cast<const TreeView::RootNode*>(node.get()))
                                nodeIcon = rootIcon_;
                            else if (auto dir = dynamic_cast<const TreeView::DirNode*>(node.get()))
                            {
                                nodeIcon = dirIcon_;
                                isActive = dir->folder.isActive();
                            }
                            else if (dynamic_cast<const TreeView::FilesNode*>(node.get()))
                                nodeIcon = fileIcon_;

                            drawIcon(nodeIcon, rectTmp, isActive);

                            rectTmp.x     += widthNodeIcon_ + gridGap_;
                            rectTmp.width -= widthNodeIcon_ + gridGap_;

                            if (rectTmp.width > 0)
                            {
                                if (!isActive)
                                    textColor.Set(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));

                                drawCellText(dc, rectTmp, getValue(row, colType), wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
                            }
                        }
                    }
                }
            }
        }
        else
        {
            int alignment = wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL;

            //have file size and item count right-justified (but don't change for RTL languages)
            if ((static_cast<ColumnTypeTree>(colType) == ColumnTypeTree::bytes ||
                 static_cast<ColumnTypeTree>(colType) == ColumnTypeTree::itemCount) && grid_.GetLayoutDirection() != wxLayout_RightToLeft)
            {
                rectTmp.width -= 2 * gridGap_;
                alignment = wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL;
            }
            else //left-justified
            {
                rectTmp.x     += 2 * gridGap_;
                rectTmp.width -= 2 * gridGap_;
            }

            drawCellText(dc, rectTmp, getValue(row, colType), alignment);
        }
    }

    int getBestSize(wxDC& dc, size_t row, ColumnType colType) override
    {
        // -> synchronize renderCell() <-> getBestSize() <-> getRowMouseHover()

        if (static_cast<ColumnTypeTree>(colType) == ColumnTypeTree::folder)
        {
            if (std::unique_ptr<TreeView::Node> node = getDataView().getLine(row))
                return node->level_ * widthLevelStep_ + gridGap_ + (showPercentBar_ ? percentageBarWidth_ + 2 * gridGap_ : 0) + widthNodeStatus_ + gridGap_
                       + widthNodeIcon_ + gridGap_ + dc.GetTextExtent(getValue(row, colType)).GetWidth() +
                       gridGap_; //additional gap from right
            else
                return 0;
        }
        else
            return 2 * gridGap_ + dc.GetTextExtent(getValue(row, colType)).GetWidth() +
                   2 * gridGap_; //include gap from right!
    }

    HoverArea getRowMouseHover(wxDC& dc, size_t row, ColumnType colType, int cellRelativePosX, int cellWidth) override
    {
        switch (static_cast<ColumnTypeTree>(colType))
        {
            case ColumnTypeTree::folder:
                if (std::unique_ptr<TreeView::Node> node = getDataView().getLine(row))
                {
                    const int tolerance = 2;
                    const int nodeStatusXFirst = -tolerance + static_cast<int>(node->level_) * widthLevelStep_ + gridGap_ + (showPercentBar_ ? percentageBarWidth_ + 2 * gridGap_ : 0);
                    const int nodeStatusXLast  = (nodeStatusXFirst + tolerance) + widthNodeStatus_ + tolerance;
                    // -> synchronize renderCell() <-> getBestSize() <-> getRowMouseHover()

                    if (nodeStatusXFirst <= cellRelativePosX && cellRelativePosX < nodeStatusXLast)
                        return static_cast<HoverArea>(HoverAreaTree::node);
                }
                break;

            case ColumnTypeTree::itemCount:
            case ColumnTypeTree::bytes:
                break;
        }
        return HoverArea::none;
    }

    std::wstring getColumnLabel(ColumnType colType) const override
    {
        switch (static_cast<ColumnTypeTree>(colType))
        {
            case ColumnTypeTree::folder:
                return _("Folder");
            case ColumnTypeTree::itemCount:
                return _("Items");
            case ColumnTypeTree::bytes:
                return _("Size");
        }
        return std::wstring();
    }

    void onMouseLeft(GridClickEvent& event)
    {
        switch (static_cast<HoverAreaTree>(event.hoverArea_))
        {
            case HoverAreaTree::node:
                switch (getDataView().getStatus(event.row_))
                {
                    case TreeView::STATUS_EXPANDED:
                        return reduceNode(event.row_);
                    case TreeView::STATUS_REDUCED:
                        return expandNode(event.row_);
                    case TreeView::STATUS_EMPTY:
                        break;
                }
                break;
        }
        event.Skip();
    }

    void onMouseLeftDouble(GridClickEvent& event)
    {
        switch (getDataView().getStatus(event.row_))
        {
            case TreeView::STATUS_EXPANDED:
                return reduceNode(event.row_);
            case TreeView::STATUS_REDUCED:
                return expandNode(event.row_);
            case TreeView::STATUS_EMPTY:
                break;
        }
        event.Skip();
    }

    void onKeyDown(wxKeyEvent& event)
    {
        int keyCode = event.GetKeyCode();
        if (grid_.GetLayoutDirection() == wxLayout_RightToLeft)
        {
            if (keyCode == WXK_LEFT || keyCode == WXK_NUMPAD_LEFT)
                keyCode = WXK_RIGHT;
            else if (keyCode == WXK_RIGHT || keyCode == WXK_NUMPAD_RIGHT)
                keyCode = WXK_LEFT;
        }

        const size_t rowCount = grid_.getRowCount();
        if (rowCount == 0) return;

        const size_t row = grid_.getGridCursor();
        if (event.ShiftDown())
            ;
        else if (event.ControlDown())
            ;
        else
            switch (keyCode)
            {
                case WXK_LEFT:
                case WXK_NUMPAD_LEFT:
                case WXK_NUMPAD_SUBTRACT: //https://docs.microsoft.com/en-us/previous-versions/windows/desktop/dnacc/guidelines-for-keyboard-user-interface-design#windows-shortcut-keys
                    switch (getDataView().getStatus(row))
                    {
                        case TreeView::STATUS_EXPANDED:
                            return reduceNode(row);
                        case TreeView::STATUS_REDUCED:
                        case TreeView::STATUS_EMPTY:

                            const int parentRow = getDataView().getParent(row);
                            if (parentRow >= 0)
                                grid_.setGridCursor(parentRow, GridEventPolicy::allow);
                            break;
                    }
                    return; //swallow event

                case WXK_RIGHT:
                case WXK_NUMPAD_RIGHT:
                case WXK_NUMPAD_ADD:
                    switch (getDataView().getStatus(row))
                    {
                        case TreeView::STATUS_EXPANDED:
                            grid_.setGridCursor(std::min(rowCount - 1, row + 1), GridEventPolicy::allow);
                            break;
                        case TreeView::STATUS_REDUCED:
                            return expandNode(row);
                        case TreeView::STATUS_EMPTY:
                            break;
                    }
                    return; //swallow event
            }

        event.Skip();
    }

    void onGridLabelContext(GridLabelClickEvent& event)
    {
        ContextMenu menu;
        //--------------------------------------------------------------------------------------------------------
        menu.addCheckBox(_("Percentage"), [this] { setShowPercentage(!getShowPercentage()); }, getShowPercentage());
        //--------------------------------------------------------------------------------------------------------
        auto toggleColumn = [&](ColumnType ct)
        {
            auto colAttr = grid_.getColumnConfig();

            Grid::ColAttributes* caFolderName = nullptr;
            Grid::ColAttributes* caToggle     = nullptr;

            for (Grid::ColAttributes& ca : colAttr)
                if (ca.type == static_cast<ColumnType>(ColumnTypeTree::folder))
                    caFolderName = &ca;
                else if (ca.type == ct)
                    caToggle = &ca;

            assert(caFolderName && caFolderName->stretch > 0 && caFolderName->visible);
            assert(caToggle     && caToggle->stretch == 0);

            if (caFolderName && caToggle)
            {
                caToggle->visible = !caToggle->visible;

                //take width of newly visible column from stretched folder name column
                caFolderName->offset -= caToggle->visible ? caToggle->offset : -caToggle->offset;

                grid_.setColumnConfig(colAttr);
            }
        };

        for (const Grid::ColAttributes& ca : grid_.getColumnConfig())
        {
            menu.addCheckBox(getColumnLabel(ca.type), [ct = ca.type, toggleColumn] { toggleColumn(ct); },
                             ca.visible, ca.type != static_cast<ColumnType>(ColumnTypeTree::folder)); //do not allow user to hide file name column!
        }
        //--------------------------------------------------------------------------------------------------------
        menu.addSeparator();

        auto setDefaultColumns = [&]
        {
            setShowPercentage(treeGridShowPercentageDefault);
            grid_.setColumnConfig(convertColAttributes(getTreeGridDefaultColAttribs(), getTreeGridDefaultColAttribs()));
        };
        menu.addItem(_("&Default"), setDefaultColumns); //'&' -> reuse text from "default" buttons elsewhere
        //--------------------------------------------------------------------------------------------------------

        menu.popup(grid_);
        //event.Skip();
    }

    void onGridLabelLeftClick(GridLabelClickEvent& event)
    {
        const auto colTypeTree = static_cast<ColumnTypeTree>(event.colType_);

        bool sortAscending = getDefaultSortDirection(colTypeTree);
        const auto [sortCol, ascending] = getDataView().getSortConfig();
        if (sortCol == colTypeTree)
            sortAscending = !ascending;

        getDataView().setSortDirection(colTypeTree, sortAscending);
        grid_.clearSelection(GridEventPolicy::allow);
        grid_.Refresh();
    }

    void expandNode(size_t row)
    {
        getDataView().expandNode(row);
        grid_.Refresh(); //implicitly clears selection (changed row count after expand)
        grid_.setGridCursor(row, GridEventPolicy::allow);
        //grid_.autoSizeColumns(); -> doesn't look as good as expected
    }

    void reduceNode(size_t row)
    {
        getDataView().reduceNode(row);
        grid_.Refresh();
        grid_.setGridCursor(row, GridEventPolicy::allow);
    }

    SharedRef<TreeView> treeDataView_ = makeSharedRef<TreeView>();

    const int gridGap_            = fastFromDIP(TREE_GRID_GAP_SIZE_DIP);
    const int percentageBarWidth_ = fastFromDIP(PERCENTAGE_BAR_WIDTH_DIP);

    const wxImage fileIcon_ = IconBuffer::genericFileIcon(IconBuffer::SIZE_SMALL);
    const wxImage dirIcon_  = IconBuffer::genericDirIcon (IconBuffer::SIZE_SMALL);

    const int widthNodeIcon_;
    const int widthLevelStep_;
    const int widthNodeStatus_;

    const wxImage rootIcon_;

    Grid& grid_;
    bool showPercentBar_ = true;
};
}


void treegrid::init(Grid& grid)
{
    grid.setDataProvider(std::make_shared<GridDataTree>(grid));
    grid.showRowLabel(false);

    const int rowHeight = std::max(IconBuffer::getSize(IconBuffer::SIZE_SMALL) + 2, //1 extra pixel on top/bottom; dearly needed on OS X!
                                   grid.getMainWin().GetCharHeight()); //seems to already include 3 margin pixels on top/bottom (consider percentage area)
    grid.setRowHeight(rowHeight);
}


void treegrid::setData(zen::Grid& grid, FolderComparison& folderCmp)
{
    if (auto* prov = dynamic_cast<GridDataTree*>(grid.getDataProvider()))
        return prov->setData(folderCmp);
    throw std::runtime_error(std::string(__FILE__) + '[' + numberTo<std::string>(__LINE__) + "] treegrid was not initialized.");
}


TreeView& treegrid::getDataView(Grid& grid)
{
    if (auto* prov = dynamic_cast<GridDataTree*>(grid.getDataProvider()))
        return prov->getDataView();
    throw std::runtime_error(std::string(__FILE__) + '[' + numberTo<std::string>(__LINE__) + "] treegrid was not initialized.");
}


void treegrid::setShowPercentage(Grid& grid, bool value)
{
    if (auto* prov = dynamic_cast<GridDataTree*>(grid.getDataProvider()))
        prov->setShowPercentage(value);
    else
        assert(false);
}


bool treegrid::getShowPercentage(const Grid& grid)
{
    if (auto* prov = dynamic_cast<const GridDataTree*>(grid.getDataProvider()))
        return prov->getShowPercentage();
    assert(false);
    return true;
}
