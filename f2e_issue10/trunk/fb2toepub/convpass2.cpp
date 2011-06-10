//
//  Copyright (C) 2010, 2011 Alexey Bobkov
//
//  This file is part of Fb2toepub converter.
//
//  Fb2toepub converter is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  Fb2toepub converter is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with Fb2toepub converter.  If not, see <http://www.gnu.org/licenses/>.
//


#include "hdr.h"

#include "scanner.h"
#include "scandir.h"
#include "converter.h"
#include "base64.h"
#include "uuidmisc.h"
#include "mangling.h"
#include "opentypefont.h"
//#include <streambuf>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <ctype.h>

#include "fb2parser.h"
#include "xmlwriter.h"

namespace Fb2ToEpub
{

//-----------------------------------------------------------------------
namespace Pass2
{


const std::size_t THRESHOLD_SIZE = 0x6000UL;


// SOME DAY I WILL IMPLEMENT IT (MAYBE)
/*
class OStreambuf : public std::basic_streambuf<char>
{
public:
    explicit OStreambuf(OutStm *pout, std::size_t buff_sz = 256) :
        buffer_(buff_sz + 1)
    {
        char *base = &buffer_.front();
        setp(base, base + buffer_.size() - 1); // -1 to make overflow() easier
    }

private:
    int_type overflow(int_type ch);
    int sync();

private:
    std::vector<char> buffer_;
};
*/

class SetLanguage
{
    String *pstr_, old_;
public:
    SetLanguage(String *pstr, const AttrMap &attrmap) : pstr_(pstr), old_(*pstr)
    {
        AttrMap::const_iterator cit = attrmap.find("xml:lang");
        if(cit != attrmap.end())
            *pstr_ = cit->second;
    }
    ~SetLanguage()
    {
        *pstr_ = old_;
    }
};


//-----------------------------------------------------------------------
static String EncodeStr(const String &str)
{
    std::vector<char> buf;
    LexScanner::Encode(str.c_str(), &buf);
    return &buf[0];
}


//-----------------------------------------------------------------------
// PASS 2 ENGINE
//-----------------------------------------------------------------------
class Engine : Noncopyable
{
public:
    Engine (LexScanner *scanner,
            const strvector &css,
            const strvector &fonts,
            const strvector &mfonts,
            XlitConv *xlitConv,
            UnitArray *units,
            OutPackStm *pout)
                        :   s_                  (scanner),
                            css_                (css),
                            fonts_              (fonts),
                            mfonts_             (mfonts),
                            xlitConv_           (xlitConv),
                            units_              (*units),
                            pout_               (pout),
                            tocLevels_          (0),
                            coverPgIt_          (units->end()),
                            //coverBinIdx_        (-1),
                            uniqueIdIdx_        (0),
                            //unitIdx_            (0),
                            //unitActive_         (false),
                            //unitHasId_          (false),

                            sectionSize_        (0)
    {
    }

    void Start()    
    {
        BuildOutputLayout();
        {
            std::set<String> noteRefIds;
            BuildReferenceMaps(&noteRefIds);
            BuildAnchors(noteRefIds);
        }

#if 0
#if defined(_DEBUG)
        {
            for(std::size_t i = 0; i < units_.size(); ++i)
                printf ("%d %d-%d-%d %s size=%d, parent=%d, level = %d, %s.xhtml, noteRefId = \"%s\"\n", i, units_[i].bodyType_, units_[i].type_,
                        units_[i].id_, units_[i].title_.c_str(), units_[i].size_, units_[i].parent_, units_[i].level_, units_[i].file_.c_str(), units_[i].noteRefId_.c_str());

            for(ReferenceMap::const_iterator cit = refidToNew_.begin(), cit_end = refidToNew_.end(); cit != cit_end; ++cit)
                printf("%s -> %s\n", cit->first.c_str(), cit->second.c_str());
            for(RefidInfoMap::const_iterator cit = refidToUnit_.begin(), cit_end = refidToUnit_.end(); cit != cit_end; ++cit)
                printf("%s in %s\n", cit->first.c_str(), cit->second->file_.c_str());
            for(ReferenceMap::const_iterator cit = noteidToAnchorId_.begin(), cit_end = noteidToAnchorId_.end(); cit != cit_end; ++cit)
                printf("%s -> anchor %s\n", cit->first.c_str(), cit->second.c_str());
        }
        printf("\n\n\n\n\n\n\n\n");
#endif
#endif

        // start epub
        AddMimetype();
        AddContainer();

        // scan all embedded fonts
        ScanFonts("ttf", &ttffiles_);
        ScanFonts("otf", &otffiles_);

        // add encryption.xml
        AddEncryption();

        // scan and add stylesheet files
        AddStyles();
    }

    void End()
    {
        // add font files
        AddFontFiles(ttffiles_);
        AddFontFiles(otffiles_);

        // rest of epub
        MakeCoverPageFirst();
        //AddContentOpf();
        //AddTocNcx();
    }

    void SetBookTitle           (const String &title)   {title_ = title;}
    void AddAuthor              (const String &author)  {authors_.push_back(author);}
    void SetBookId              (const String &id);
    void SetIsbn                (const String &isbn)    {isbn_ = isbn;}

    void StartXmlFrame          ();
    void EndXmlFrame            ();
    Ptr<XmlWriter> Writer       ()                      {return pwrt_;}
    void AddRefId               (Fb2Host *host);
    void AddRef                 (Fb2Host *host);

private:
    Ptr<LexScanner>         s_;
    const strvector         &css_, &fonts_, &mfonts_;
    Ptr<XlitConv>           xlitConv_;
    UnitArray               &units_;
    Ptr<OutPackStm>         pout_;
    Ptr<XmlWriter>          pwrt_;
    std::vector<int>        xmlFrames_;

    struct Binary
    {
        String file_, type_;
        Binary() {}
        Binary(const String &file, const String type) : file_(file), type_(type) {}
    };
    typedef std::vector<Binary> binvector;

    // external file - result of directory scanning
    struct ExtFile
    {
        String fname_, ospath_; // file name and OS path
        ExtFile() {}
        ExtFile(const String &fname, const String &ospath) : fname_(fname), ospath_(ospath) {}
    };
    typedef std::vector<ExtFile> ExtFileVector;

    typedef std::map<String, const Unit*> RefidInfoMap;    // reference id -> Unit containing this id

    int                     tocLevels_;         // number of levels of table of content
    UnitArray::iterator     coverPgIt_;         // pointer to unit describing cover image
    //String                  coverFile_;         // cover image file name
    //int                     coverBinIdx_;       // cover image index in binary section
    int                     uniqueIdIdx_;       // unique id counter
    ReferenceMap            refidToNew_;        // (re)mapping of original reference id to unique reference id
    RefidInfoMap            refidToUnit_;       // mapping unique reference id to unit containing this id
    ReferenceMap            noteidToAnchorId_;  // mapping of note ref id to anchor ref id
    //std::set<String>        usedAnchorsids_;    // anchor ids already set
    strvector               cssfiles_;          // all stylesheet files
    ExtFileVector           ttffiles_, otffiles_; // all font file description
    //binvector               binaries_;          // all binary files
    //std::set<String>        xlns_;              // xlink namespaces
    std::set<String>        allRefIds_;         // all ref ids

    //String                  title_, lang_, id_, id1_, title_info_date_, isbn_;  // book info
    String                  title_;
    //String                  lang_;
    String                  id_, id1_;
    //String                  title_info_date_;
    String                  isbn_;

    unsigned char           adobeKey_[16];      // adobe key
    strvector               authors_;           // book authors

    //String                  prevUnitFile_;
    //int                     unitIdx_;
    //bool                    unitActive_;
    //bool                    unitHasId_;
    std::size_t             sectionSize_;
    //String                  bodyXmlLang_, sectXmlLang_;


    void AdjustUnitSizes        ();
    void CalcTocLevels          ();
    int CalcLevelToSplit        ();
    String MakeUniqueId         (bool anchor = false);
    void BuiltFileLayout        (int levelToSplit);
    void BuildOutputLayout      ();
    void BuildReferenceMaps     (std::set<String> *noteRefIds);
    void BuildAnchors           (const std::set<String> &noteRefIds);

    //String Findhref             (const AttrMap &attrmap) const;

    //void StartUnit              (Unit::Type unitType, AttrMap *attrmap = NULL);
    //void EndUnit                ();
    //void SwitchUnitIfSizeAbove  (std::size_t size);

    void AddMimetype            ();
    void AddContainer           ();
    void ScanFonts              (const char *ext, ExtFileVector *fontfiles);
    void AddEncryption          ();
    void AddStyles              ();
    void AddFontFiles           (const ExtFileVector &fontfiles);    
    void MakeCoverPageFirst     ();
    //void AddContentOpf          ();
    //void AddTocNcx              ();
    //const String* AddId         (const AttrMap &attrmap);
    //void ParseTextAndEndElement (const String &element);
    //void CopyAttribute          (const String &attr, const AttrMap &attrmap);
    //void CopyXmlLang            (const AttrMap &attrmap);
    //bool AddAnchorid            (const String &anchorid);
};

//-----------------------------------------------------------------------
void Engine::SetBookId(const String &id)
{
    static const String uuidpfx = "urn:uuid:";

    String uuid = id;
    if(!uuid.compare(0, uuidpfx.length(), uuidpfx))
        uuid = uuid.substr(uuidpfx.length());
    if(!IsValidUUID(uuid))
    {
        id1_ = id;
        uuid = GenerateUUID();
    }

    id_ = uuidpfx + uuid;
    MakeAdobeKey(uuid, adobeKey_);
}

//-----------------------------------------------------------------------
void Engine::StartXmlFrame()
{
    xmlFrames_.push_back(pwrt_->ElementNumber());
}

//-----------------------------------------------------------------------
void Engine::EndXmlFrame()
{
    pwrt_->EndElements(pwrt_->ElementNumber() - xmlFrames_.back());
    xmlFrames_.pop_back();
}

//-----------------------------------------------------------------------
void Engine::AddRefId(Fb2Host *host)
{
    const AttrMap &attrmap = host->GetAttributes();
    AttrMap::const_iterator cit = attrmap.find("id");
    if(cit == attrmap.end())
        return;

    if(allRefIds_.find(cit->second) != allRefIds_.end())
        return;    // ignore second instance

    allRefIds_.insert(cit->second);

    String id = cit->second;

    // remap it to our new id
    id = refidToNew_[id];
    if(id.empty())
        InternalError(__FILE__, __LINE__, "AddId error");

    //pout_->WriteFmt(" id=\"%s\"", EncodeStr(id).c_str());
}

//-----------------------------------------------------------------------
void Engine::AddRef(Fb2Host *host)
{
}

//-----------------------------------------------------------------------
void Engine::AdjustUnitSizes()
{
    for(int i = units_.size(); --i >= 0;)
    {
        Unit &unit = units_[i];
        if(unit.parent_ < 0)
            continue;
#if defined(_DEBUG)
        if(unit.parent_ > i)
            InternalError(__FILE__, __LINE__, "wrong unit order");
#endif
        units_[unit.parent_].size_ += unit.size_;
    }
}

//-----------------------------------------------------------------------
void Engine::CalcTocLevels()
{
    int levels = 0;
    UnitArray::iterator it = units_.begin(), it_end = units_.end();
    for(; it < it_end; ++it)
    {
#if !FB2TOEPUB_SUPPRESS_EMPTY_TITLES
        if(it->title_.empty() && it->bodyType_ != Unit::BODY_NONE)
            it->title_ = "- - - - -";
#endif
        if(it->parent_ < 0)
            it->level_ = 0;
        else
        {
            int parentLevel = units_[it->parent_].level_;
            int level = units_[it->parent_].title_.empty() ? parentLevel : parentLevel + 1;
            it->level_ = level;
            if(levels < level)
                levels = level;
        }
    }

    tocLevels_ = levels+1;
}

typedef std::vector<std::size_t> SizeVector;

//-----------------------------------------------------------------------
int Engine::CalcLevelToSplit()
{
    // calc max size for each level
    SizeVector maxLevelSize(tocLevels_, 0);
    {
        UnitArray::iterator it = units_.begin(), it_end = units_.end();
        for(; it < it_end; ++it)
        {
#if defined(_DEBUG)
            if(it->level_ >= tocLevels_)
                InternalError(__FILE__, __LINE__, "incorrect level");
#endif
            if(maxLevelSize[it->level_] < it->size_)
                maxLevelSize[it->level_] = it->size_;
        }
    }

    for(int i = maxLevelSize.size(); --i >= 0;)
        if(maxLevelSize[i] > THRESHOLD_SIZE)
            return i;
    return 0;
}

//-----------------------------------------------------------------------
static String MakeFileName(const String &prefix, int idx)
{
    std::ostringstream fileName;
    fileName << prefix;
    fileName.width(4);
    fileName.fill('0');
    fileName << idx;
    return fileName.str();
}

//-----------------------------------------------------------------------
String Engine::MakeUniqueId(bool anchor)
{
    std::ostringstream fileId;
    fileId << (anchor ? "anchor" : "id") << uniqueIdIdx_++;
    return fileId.str();
}

//-----------------------------------------------------------------------
void Engine::BuiltFileLayout(int levelToSplit)
{
    // find cover page
    UnitArray::iterator it = units_.begin(), it_end = units_.end();
    for(; it < it_end; ++it)
        switch(it->type_)
        {
        case Unit::COVERPAGE:   coverPgIt_ = it; break;
        //case Unit::IMAGE:       coverPgIt_ = it; break;
        case Unit::SECTION:     break;
        default:                continue;
        }

    // build layout
    int fileIdx = 0;
    String file;
    int prevLevel = -1;
    Unit::Type prevType = Unit::UNIT_NONE;
    for(it = units_.begin(); it < it_end; ++it)
    {
#if FB2TOEPUB_TOC_REFERS_FILES_ONLY
        if ((it->type_ != prevType && (prevType != Unit::TITLE || it->type_ != Unit::SECTION)) || it->level_ <= levelToSplit)
#else
        if ((it->type_ != prevType && (prevType != Unit::TITLE || it->type_ != Unit::SECTION)) ||
            (it->level_ <= levelToSplit && prevLevel >= levelToSplit) ||
            (it->level_ <= prevLevel && prevLevel <= levelToSplit))
#endif
        {
            if(it == coverPgIt_)
                file = "cover";
            else
                file = MakeFileName("txt", fileIdx++);
        }

#if FB2TOEPUB_TOC_REFERS_FILES_ONLY
        // force exclusion from TOC for all units above split level
        if(it->level_ > levelToSplit)
            it->title_.clear();
#endif

        // assing unit file name
        it->file_ = file;

#if !FB2TOEPUB_TOC_REFERS_FILES_ONLY
        // make and assing new unit id
        it->fileId_ = MakeUniqueId();
#endif

        prevLevel = it->level_;
        prevType = it->type_;
    }
}

//-----------------------------------------------------------------------
void Engine::BuildOutputLayout()
{
    // adjust sizes
    AdjustUnitSizes();

    // calculate # of toc levels
    CalcTocLevels();

    // determine which level will be splitted to different files, and build file layout
    BuiltFileLayout(CalcLevelToSplit());
}

//-----------------------------------------------------------------------
void Engine::BuildReferenceMaps(std::set<String> *noteRefIds)
{
    ReferenceMap::iterator refidToNew_end = refidToNew_.end();
    RefidInfoMap::iterator refidToUnit_end = refidToUnit_.end();

    UnitArray::const_iterator cit = units_.begin(), cit_end = units_.end();
    for(; cit < cit_end; ++cit)
    {
        strvector::const_iterator cit1 = cit->refIds_.begin(), cit1_end = cit->refIds_.end();
        for(; cit1 < cit1_end; ++cit1)
        {
            const String &id = *cit1;

            // map original id to new id
            String newId;
            {
                ReferenceMap::iterator it = refidToNew_.lower_bound(id);
                if(it != refidToNew_end && it->first == id)
                    newId = it->second;
                else
                {
                    newId = MakeUniqueId();
                    refidToNew_.insert(it, ReferenceMap::value_type(id, newId));
                }
            }

            // map new unique id to unit
            {
                // The input unit array should not contatin duplicate ids.
                // Actually we are supposed to skip all dup refs in AddId method, pass 1.
                // So if we found one then it is internal error.
                RefidInfoMap::iterator it = refidToUnit_.lower_bound(newId);
#if defined(_DEBUG)
                if(it != refidToUnit_end && it->first == id)
                    InternalError(__FILE__, __LINE__, "duplicate reference id");
#endif
                refidToUnit_.insert(it, RefidInfoMap::value_type(newId, &*cit));
            }
        }

        // store note id
        if(!cit->noteRefId_.empty())
            noteRefIds->insert(cit->noteRefId_);
    }
}

//-----------------------------------------------------------------------
void Engine::BuildAnchors(const std::set<String> &noteRefIds)
{
    UnitArray::const_iterator cit = units_.begin(), cit_end = units_.end();
    for(; cit < cit_end; ++cit)
    {
        std::set<String>::const_iterator cit1 = cit->refs_.begin(), cit1_end = cit->refs_.end();
        for(; cit1 != cit1_end; ++cit1)
            if(noteRefIds.find(*cit1) != noteRefIds.end())
            {
                // this is id to note/comment section
                String id = refidToNew_[*cit1];

                // make sure that this id appears first time
                ReferenceMap::iterator it = noteidToAnchorId_.lower_bound(id);
                if(it != noteidToAnchorId_.end() && it->first == id)
                    continue;   // already has anchor

                // create new unique anchor id
                String anchorid = MakeUniqueId(true);
                refidToUnit_[anchorid] = &*cit;
                noteidToAnchorId_.insert(it, ReferenceMap::value_type(id, anchorid));
            }
    }
}

//-----------------------------------------------------------------------
void Engine::AddMimetype()
{
    static const char contents[] = "application/epub+zip";
    pout_->BeginFile("mimetype", false);
    pout_->Write(contents, sizeof(contents)/sizeof(char)-1);
}

//-----------------------------------------------------------------------
void Engine::AddContainer()
{
    static const char contents[] =  "<?xml version=\"1.0\"?>\n"
                                    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                                    "  <rootfiles>\n"
                                    "    <rootfile full-path=\"OPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
                                    "  </rootfiles>\n"
                                    "</container>";

    pout_->BeginFile("META-INF/container.xml", true);
    pout_->Write(contents, sizeof(contents)/sizeof(char)-1);
}

//-----------------------------------------------------------------------
void Engine::ScanFonts(const char *ext, ExtFileVector *fontfiles)
{
    strvector::const_iterator cit = fonts_.begin(), cit_end = fonts_.end();
    for(; cit < cit_end; ++cit)
    {
        Ptr<ScanDir> sd = CreateScanDir(cit->c_str(), ext);
        String fname;
        for(String ospath = sd->GetNextFile(&fname); !ospath.empty(); ospath = sd->GetNextFile(&fname))
            fontfiles->push_back(ExtFile(String("fonts/") + fname, ospath));
    }
}

//-----------------------------------------------------------------------
void Engine::AddEncryption()
{
    if(ttffiles_.empty() && otffiles_.empty())
        return;

    pout_->BeginFile("META-INF/encryption.xml", true);
    Ptr<XmlWriter> wrt = CreateXmlWriter(pout_, "UTF-8");

    wrt->StartElement("encryption", true, true, "xmlns", "urn:oasis:names:tc:opendocument:xmlns:container");

    ExtFileVector::const_iterator cit, cit_end;
    for(cit = ttffiles_.begin(), cit_end = ttffiles_.end(); cit < cit_end; ++cit)
    {
        XmlFrame frm(wrt);
        wrt->StartElement("EncryptedData",      true, true, "xmlns", "http://www.w3.org/2001/04/xmlenc#");
        wrt->EmptyElement("EncryptionMethod",   true,       "Algorithm", "http://ns.adobe.com/pdf/enc#RC");
        wrt->StartElement("CipherData",         true, true);
        wrt->EmptyElement("CipherReference",    true,       "URI", String("OPS/") + cit->fname_);
        frm.End();
    }

    for(cit = otffiles_.begin(), cit_end = otffiles_.end(); cit < cit_end; ++cit)
    {
        XmlFrame frm(wrt);
        wrt->StartElement("EncryptedData",      true, true, "xmlns", "http://www.w3.org/2001/04/xmlenc#");
        wrt->EmptyElement("EncryptionMethod",   true,       "Algorithm", "http://ns.adobe.com/pdf/enc#RC");
        wrt->StartElement("CipherData",         true, true);
        wrt->EmptyElement("CipherReference",    true,       "URI", String("OPS/") + cit->fname_);
        frm.End();
    }

    wrt->Flush();
}

//-----------------------------------------------------------------------
void Engine::AddStyles()
{
    strvector::const_iterator cit = css_.begin(), cit_end = css_.end();
    for(; cit < cit_end; ++cit)
    {
        Ptr<ScanDir> sd = CreateScanDir(cit->c_str(), "css");
        String fname;
        for(String ospath = sd->GetNextFile(&fname); !ospath.empty(); ospath = sd->GetNextFile(&fname))
        {
            fname = String("css/") + fname;
            pout_->AddFile(CreateInFileStm(ospath.c_str()), (String("OPS/") + fname).c_str(), true);
            cssfiles_.push_back(fname);
        }
    }
}

//-----------------------------------------------------------------------
void Engine::AddFontFiles(const ExtFileVector &fontfiles)
{
    ExtFileVector::const_iterator cit = fontfiles.begin(), cit_end = fontfiles.end();
    for(; cit < cit_end; ++cit)
        if(!IsFontEmbedAllowed(cit->ospath_))
            FontError(cit->ospath_, "embedding not allowed");

    for(cit = fontfiles.begin(); cit < cit_end; ++cit)
    {
        // mangle (mangling == deflating + XORing), then store without compression
        Ptr<InStm> stm = CreateManglingStm(CreateInFileStm(cit->ospath_.c_str()), adobeKey_, sizeof(adobeKey_), 1024);
        pout_->AddFile(stm, (String("OPS/") + cit->fname_).c_str(), false);

        // just compress
        //Ptr<InStm> stm = CreateInFileStm(cit->ospath_.c_str());
        //pout_->AddFile(stm, (String("OPS/") + cit->fname_).c_str(), true);
    }
}

//-----------------------------------------------------------------------
void Engine::MakeCoverPageFirst()
{
    if(coverPgIt_ != units_.end())
    {
        // move cover to begin
        Unit coverUnit = *coverPgIt_;
        units_.erase(coverPgIt_);
        units_.insert(units_.begin(), coverUnit);
        coverPgIt_ = units_.begin();
    }
}


//-----------------------------------------------------------------------
// EPUB ELEMENT DATABASE
//-----------------------------------------------------------------------
enum EpubEType
{
    EPUB_A,
    EPUB_CODE,
    EPUB_DEL,
    EPUB_DIV,
    EPUB_DIVNL,
    EPUB_EM,
    EPUB_H1,
    EPUB_H2,
    EPUB_P,
    EPUB_SPAN,
    EPUB_STRONG,
    EPUB_SUB,
    EPUB_SUP,
    EPUB_TABLE,
    EPUB_TD,
    EPUB_TH,
    EPUB_TR,
    
    EPUB_COUNT
};
//-----------------------------------------------------------------------
// XML ELEMENT INFO
struct EpubElementInfo
{
    String  name_;          // element name and class
    bool    startLn_;       // new line after start tag?
    bool    endLn_;         // new line after end tag?

    EpubElementInfo() {}
    EpubElementInfo(const String &name, bool startLn, bool endLn)
        : name_(name), startLn_(startLn), endLn_(endLn) {}
};
static const EpubElementInfo epubeinfo[EPUB_COUNT] =
{
    EpubElementInfo("a",        false,  false),
    EpubElementInfo("code",     false,  false),
    EpubElementInfo("del",      false,  false),
    EpubElementInfo("div",      false,  true),
    EpubElementInfo("div",      true,   true),
    EpubElementInfo("em",       false,  false),
    EpubElementInfo("h1",       false,  true),
    EpubElementInfo("h2",       false,  true),
    EpubElementInfo("p",        false,  true),
    EpubElementInfo("span",     false,  false),
    EpubElementInfo("strong",   false,  false),
    EpubElementInfo("sub",      false,  false),
    EpubElementInfo("sup",      false,  false),
    EpubElementInfo("table",    false,  true),
    EpubElementInfo("td",       false,  true),
    EpubElementInfo("th",       false,  true),
    EpubElementInfo("tr",       false,  true)
};



//-----------------------------------------------------------------------
// BASE PASS 2 HANDLER (WRITE ELEMENT TAGS, CLASS,
// ADD REF IDS OR HREF IF NECESSARY)
//-----------------------------------------------------------------------
template <bool skipRest = false>
class BaseHandler : public Fb2BaseEHandler<skipRest>
{
    Engine      *engine_;
    String      element_, cls_;
    bool        startLn_, endLn_;
protected:
    Engine* GetEngine() const   {return engine_;}
public:
    BaseHandler(Engine *engine,
                const String &element,
                const String &cls,
                bool startLn,
                bool endLn)
                    :   engine_ (engine),
                        element_(element),
                        cls_    (cls),
                        startLn_(startLn),
                        endLn_  (endLn)
    {
    }
    BaseHandler(Engine *engine, EpubEType type, const String &cls)
                    :   engine_ (engine),
                        element_(epubeinfo[type].name_),
                        cls_    (cls),
                        startLn_(epubeinfo[type].startLn_),
                        endLn_  (epubeinfo[type].endLn_)
    {
    }

    //virtuals
    bool StartTag(Fb2EType type, Fb2Host *host)
    {
        engine_->StartXmlFrame();
        engine_->Writer()->StartElement(element_, startLn_, endLn_, "class", cls_);

        const Fb2ElementInfo *info = Fb2GetElementInfo(type);
        if(info->refid_)
            engine_->AddRefId(host);
        else if(info->ref_)
            engine_->AddRef(host);
        return Fb2BaseEHandler<skipRest>::StartTag(type, host);
    }
    void Data(const String &data, size_t)
    {
        engine_->Writer()->WriteStr(data.c_str());
    }
    bool EndTag(bool empty, Fb2Host *host)
    {
        engine_->EndXmlFrame();
        return Fb2BaseEHandler<skipRest>::EndTag(empty, host);
    }
};


//-----------------------------------------------------------------------
// TITLE-INFO CONTEXT
//-----------------------------------------------------------------------
class StoreText : public Fb2BaseEHandler<false>
{
    typedef void (Engine::*StoreTextFn)(const String &text);
    Engine      *engine_;
    StoreTextFn fn_;
public:
    StoreText(Engine *engine, StoreTextFn fn) : engine_(engine), fn_(fn) {}

    //virtuals
    void Data(const String &data, size_t)   {(engine_->*fn_)(data);}
};


//-----------------------------------------------------------------------
// TITLE-INFO CONTEXT
//-----------------------------------------------------------------------
class TitleInfoCtxt : public Fb2Ctxt, Noncopyable
{
    Ptr<Fb2Ctxt>        oldCtxt_;
    Engine              *engine_;
    Ptr<Fb2TextHandler> authorname_;

    class Author : public Fb2EHandler, Noncopyable
    {
    public:
        Author(Fb2TextHandler *pname, Engine *engine) : engine_(engine), pname_(pname) {}

        //virtuals
        bool StartTag(Fb2EType, Fb2Host*)   {pname_->Reset(); return false;}
        void Data(const String&, size_t)    {}
        bool EndTag(bool empty, Fb2Host *host)
        {
            engine_->AddAuthor(pname_->Text());
            if(!empty)
                host->Scanner()->SkipRestOfElementContent();
            return true;
        }

    private:
        Ptr<Fb2TextHandler> pname_;
        Engine              *engine_;
    };

public:
    TitleInfoCtxt(Fb2Ctxt *oldCtxt, Engine *engine)
        : oldCtxt_(oldCtxt), engine_(engine), authorname_(CreateTextEHandler(" ")) {}

    //virtual
    void GetNext(Fb2EType type, Ptr<Fb2EHandler> *h, Ptr<Fb2Ctxt> *ctxt)
    {
        switch(type)
        {
        case E_AUTHOR:
            if(h)
                *h = new Author(authorname_, engine_);
            if(ctxt)
                *ctxt = this;
            return;

        case E_FIRST_NAME:
        case E_MIDDLE_NAME:
        case E_LAST_NAME:
        case E_NICKNAME:
            if(h)
                *h = authorname_;
            if(ctxt)
                *ctxt = this;
            return;

        default:
            break;
        }

        if(!h)
        {
            oldCtxt_->GetNext(type, NULL, ctxt);
            return;
        }

        switch(type)
        {
        case E_BOOK_TITLE:
            oldCtxt_->GetNext(type, NULL, ctxt);
            *h = new StoreText(engine_, &Engine::SetBookTitle);
            break;

        /*
        case E_ANNOTATION:
            oldCtxt_->GetNext(type, NULL, ctxt);
            *h = new StartUnitHandler<>(engine_, Unit::ANNOTATION);
            break;

        case E_COVERPAGE:
            oldCtxt_->GetNext(type, NULL, ctxt);
            *h = new StartUnitHandler<>(engine_, Unit::COVERPAGE);
            break;
        */

        default:
            oldCtxt_->GetNext(type, h, ctxt);
            break;
        }
    }
};


//-----------------------------------------------------------------------
// DOCUMENT-INFO CONTEXT
//-----------------------------------------------------------------------
class DocumentInfoCtxt : public Fb2Ctxt, Noncopyable
{
    Engine *engine_;
public:
    DocumentInfoCtxt(Engine *engine) : engine_(engine) {}

    //virtual
    void GetNext(Fb2EType type, Ptr<Fb2EHandler> *h, Ptr<Fb2Ctxt> *ctxt)
    {
        if(h)
        {
            if(type == E_ID)
                *h = new StoreText(engine_, &Engine::SetBookId);
            else
                *h = CreateSkipEHandler();
        }
        if(ctxt)
            *ctxt = this;
    }
};


//-----------------------------------------------------------------------
// PUBLISH-INFO CONTEXT
//-----------------------------------------------------------------------
class PublishInfoCtxt : public Fb2Ctxt, Noncopyable
{
    Engine *engine_;
public:
    PublishInfoCtxt(Engine *engine) : engine_(engine) {}

    //virtual
    void GetNext(Fb2EType type, Ptr<Fb2EHandler> *h, Ptr<Fb2Ctxt> *ctxt)
    {
        if(h)
        {
            if(type == E_ISBN)
                *h = new StoreText(engine_, &Engine::SetIsbn);
            else
                *h = CreateSkipEHandler();
        }
        if(ctxt)
            *ctxt = this;
    }
};


};  //namespace Pass2



//-----------------------------------------------------------------------
// Pass 2
//-----------------------------------------------------------------------
void FB2TOEPUB_DECL DoConvertionPass2_new(LexScanner *scanner,
                                        const strvector &css,
                                        const strvector &fonts,
                                        const strvector &mfonts,
                                        XlitConv *xlitConv,
                                        UnitArray *units,
                                        OutPackStm *pout)
{
    Pass2::Engine engine(scanner, css, fonts, mfonts, xlitConv, units, pout);
    Ptr<Fb2StdCtxt> ctxt = CreateFb2StdCtxt();

    Ptr<Fb2EHandler> skip = CreateSkipEHandler();

    // <FictionBook>
    ctxt->RegisterHandler(E_FICTIONBOOK, CreateRootEHandler());

    // <title-info>
    {
        Ptr<Fb2Ctxt> pc = new Pass2::TitleInfoCtxt(ctxt, &engine);
        ctxt->RegisterCtxt(E_TITLE_INFO, pc);
    }
    ctxt->RegisterHandler(E_KEYWORDS, skip);

    // <document-info>
    {
        Ptr<Fb2Ctxt> pc = new Pass2::DocumentInfoCtxt(&engine);
        ctxt->RegisterCtxt(E_DOCUMENT_INFO, pc);
    }

    // <publish-info>
    {
        Ptr<Fb2Ctxt> pc = new Pass2::PublishInfoCtxt(&engine);
        ctxt->RegisterCtxt(E_PUBLISH_INFO, pc);
    }

    // skip rest of <description>
    ctxt->RegisterHandler(E_SRC_TITLE_INFO, skip);
    ctxt->RegisterHandler(E_CUSTOM_INFO,    skip);



    // parsing
    engine.Start();
    CreateFb2Parser(scanner)->Parse(ctxt);
    engine.End();
}










































const std::size_t THRESHOLD_SIZE = 0x6000UL;

class SetLanguage
{
    String *pstr_, old_;
public:
    SetLanguage(String *pstr, const AttrMap &attrmap) : pstr_(pstr), old_(*pstr)
    {
        AttrMap::const_iterator cit = attrmap.find("xml:lang");
        if(cit != attrmap.end())
            *pstr_ = cit->second;
    }
    ~SetLanguage()
    {
        *pstr_ = old_;
    }
};


//-----------------------------------------------------------------------
static String EncodeStr(const String &str)
{
    std::vector<char> buf;
    LexScanner::Encode(str.c_str(), &buf);
    return &buf[0];
}

//-----------------------------------------------------------------------
static void AddContentManifestFile(OutPackStm *pout, const String &id, const String &ref, const String &media_type)
{
    pout->WriteFmt("    <item id=\"%s\" href=\"%s\" media-type=\"%s\"/>\n", EncodeStr(id).c_str(), EncodeStr(ref).c_str(), EncodeStr(media_type).c_str());
}


//-----------------------------------------------------------------------
class ConverterPass2 : public Object, Noncopyable
{
public:
    ConverterPass2 (LexScanner *scanner,
                    const strvector &css,
                    const strvector &fonts,
                    const strvector &mfonts,
                    XlitConv *xlitConv,
                    UnitArray *units,
                    OutPackStm *pout)
                        :   s_                  (scanner),
                            css_                (css),
                            fonts_              (fonts),
                            mfonts_             (mfonts),
                            xlitConv_           (xlitConv),
                            units_              (*units),
                            pout_               (pout),
                            tocLevels_          (0),
                            coverBinIdx_        (-1),
                            uniqueIdIdx_        (0),
                            unitIdx_            (0),
                            unitActive_         (false),
                            unitHasId_          (false),
                            sectionSize_        (0)
    {
        coverPgIt_ = units_.end();
    }

    void Scan()
    {
        BuildOutputLayout();
        {
            std::set<String> noteRefIds;
            BuildReferenceMaps(&noteRefIds);
            BuildAnchors(noteRefIds);
        }

#if 0
#if defined(_DEBUG)
        {
            for(std::size_t i = 0; i < units_.size(); ++i)
                printf ("%d %d-%d-%d %s size=%d, parent=%d, level = %d, %s.xhtml, noteRefId = \"%s\"\n", i, units_[i].bodyType_, units_[i].type_,
                        units_[i].id_, units_[i].title_.c_str(), units_[i].size_, units_[i].parent_, units_[i].level_, units_[i].file_.c_str(), units_[i].noteRefId_.c_str());

            for(ReferenceMap::const_iterator cit = refidToNew_.begin(), cit_end = refidToNew_.end(); cit != cit_end; ++cit)
                printf("%s -> %s\n", cit->first.c_str(), cit->second.c_str());
            for(RefidInfoMap::const_iterator cit = refidToUnit_.begin(), cit_end = refidToUnit_.end(); cit != cit_end; ++cit)
                printf("%s in %s\n", cit->first.c_str(), cit->second->file_.c_str());
            for(ReferenceMap::const_iterator cit = noteidToAnchorId_.begin(), cit_end = noteidToAnchorId_.end(); cit != cit_end; ++cit)
                printf("%s -> anchor %s\n", cit->first.c_str(), cit->second.c_str());
        }
#endif
#endif

        // start epub
        AddMimetype();
        AddContainer();

        // scan all embedded fonts
        ScanFonts("ttf", &ttffiles_);
        ScanFonts("otf", &otffiles_);

        // add encryption.xml
        AddEncryption();

        // scan and add stylesheet files
        AddStyles();

        // perform fb2 file parsing
        s_->SkipXMLDeclaration();
        FictionBook();

        // add font files
        AddFontFiles(ttffiles_);
        AddFontFiles(otffiles_);

        // rest of epub
        MakeCoverPageFirst();
        AddContentOpf();
        AddTocNcx();
    }

private:
    Ptr<LexScanner>         s_;
    const strvector         &css_, &fonts_, &mfonts_;
    Ptr<XlitConv>           xlitConv_;
    UnitArray               &units_;
    Ptr<OutPackStm>         pout_;

    struct Binary
    {
        String file_, type_;
        Binary() {}
        Binary(const String &file, const String type) : file_(file), type_(type) {}
    };
    typedef std::vector<Binary> binvector;

    // external file - result of directory scanning
    struct ExtFile
    {
        String fname_, ospath_; // file name and OS path
        ExtFile() {}
        ExtFile(const String &fname, const String &ospath) : fname_(fname), ospath_(ospath) {}
    };
    typedef std::vector<ExtFile> ExtFileVector;

    typedef std::map<String, const Unit*> RefidInfoMap;    // reference id -> Unit containing this id

    int                     tocLevels_;         // number of levels of table of content
    UnitArray::iterator     coverPgIt_;         // pointer to unit describing cover image
    String                  coverFile_;         // cover image file name
    int                     coverBinIdx_;       // cover image index in binary section
    int                     uniqueIdIdx_;       // unique id counter
    ReferenceMap            refidToNew_;        // (re)mapping of original reference id to unique reference id
    RefidInfoMap            refidToUnit_;       // mapping unique reference id to unit containing this id
    ReferenceMap            noteidToAnchorId_;  // mapping of note ref id to anchor ref id
    std::set<String>        usedAnchorsids_;    // anchor ids already set
    strvector               cssfiles_;          // all stylesheet files
    ExtFileVector           ttffiles_, otffiles_; // all font file description
    binvector               binaries_;          // all binary files
    std::set<String>        xlns_;              // xlink namespaces
    std::set<String>        allRefIds_;         // all ref ids
    String                  title_, lang_, id_, id1_, title_info_date_, isbn_;  // book info
    unsigned char           adobeKey_[16];      // adobe key
    strvector               authors_;           // book authors

    String                  prevUnitFile_;
    int                     unitIdx_;
    bool                    unitActive_;
    bool                    unitHasId_;
    std::size_t             sectionSize_;
    String                  bodyXmlLang_, sectXmlLang_;


    void AdjustUnitSizes        ();
    void CalcTocLevels          ();
    int CalcLevelToSplit        ();
    String MakeUniqueId         (bool anchor = false);
    void BuiltFileLayout        (int levelToSplit);
    void BuildOutputLayout      ();
    void BuildReferenceMaps     (std::set<String> *noteRefIds);
    void BuildAnchors           (const std::set<String> &noteRefIds);

    String Findhref             (const AttrMap &attrmap) const;

    void StartUnit              (Unit::Type unitType, AttrMap *attrmap = NULL);
    void EndUnit                ();
    void SwitchUnitIfSizeAbove  (std::size_t size);

    void AddMimetype            ();
    void AddContainer           ();
    void AddStyles              ();
    void ScanFonts              (const char *ext, ExtFileVector *fontfiles);
    void AddFontFiles           (const ExtFileVector &fontfiles);    
    void MakeCoverPageFirst     ();
    void AddContentOpf          ();
    void AddTocNcx              ();
    void AddEncryption          ();
    const String* AddId         (const AttrMap &attrmap);
    void ParseTextAndEndElement (const String &element);
    void CopyAttribute          (const String &attr, const AttrMap &attrmap);
    void CopyXmlLang            (const AttrMap &attrmap);
    bool AddAnchorid            (const String &anchorid);

    // FictionBook elements
    void FictionBook            ();
    void a                      ();
    void annotation             (bool startUnit = false);
    void author                 ();
    void binary                 ();
    void body                   ();
    //void book_name              ();
    void book_title             ();
    void cite                   ();
    //void city                   ();
    void code                   ();
    void coverpage              ();
    //void custom_info            ();
    void date                   ();
    String date__epub           ();
    void description            ();
    void document_info          ();
    //void email                  ();
    void emphasis               ();
    void empty_line             ();
    void epigraph               ();
    //void first_name             ();
    //void genre                  ();
    //void history                ();
    //void home_page              ();
    void id                     ();
    void image                  (bool fb2_inline, bool html_inline, bool scale);
    String isbn                 ();
    //void keywords               ();
    void lang                   ();
    //void last_name              ();
    //void middle_name            ();
    //void nickname               ();
    //void output_document_class  ();
    //void output                 ();
    void p                      (const char *pelement = "p", const char *cls = NULL);
    //void part                   ();
    void poem                   ();
    //void program_used           ();
    void publish_info           ();
    //void publisher              ();
    void section                ();
    //void sequence               ();
    //void src_lang               ();
    //void src_ocr                ();
    //void src_title_info         ();
    //void src_url                ();
    void stanza                 ();
    void strikethrough          ();
    void strong                 ();
    void style                  ();
    //void stylesheet             ();
    void sub                    ();
    void subtitle               ();
    void sup                    ();
    void table                  ();
    void td                     ();
    void text_author            ();
    void th                     ();
    void title                  (bool startUnit, const String &anchorid = "");
    void title_info             ();
    void tr                     ();
    //void translator             ();
    void v                      ();
    //void version                ();
    //void year                   ();
};

//-----------------------------------------------------------------------
void ConverterPass2::AdjustUnitSizes()
{
    for(int i = units_.size(); --i >= 0;)
    {
        Unit &unit = units_[i];
        if(unit.parent_ < 0)
            continue;
#if defined(_DEBUG)
        if(unit.parent_ > i)
            InternalError(__FILE__, __LINE__, "wrong unit order");
#endif
        units_[unit.parent_].size_ += unit.size_;
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::CalcTocLevels()
{
    int levels = 0;
    UnitArray::iterator it = units_.begin(), it_end = units_.end();
    for(; it < it_end; ++it)
    {
#if !FB2TOEPUB_SUPPRESS_EMPTY_TITLES
        if(it->title_.empty() && it->bodyType_ != Unit::BODY_NONE)
            it->title_ = "- - - - -";
#endif
        if(it->parent_ < 0)
            it->level_ = 0;
        else
        {
            int parentLevel = units_[it->parent_].level_;
            int level = units_[it->parent_].title_.empty() ? parentLevel : parentLevel + 1;
            it->level_ = level;
            if(levels < level)
                levels = level;
        }
    }

    tocLevels_ = levels+1;
}

typedef std::vector<std::size_t> SizeVector;

//-----------------------------------------------------------------------
int ConverterPass2::CalcLevelToSplit()
{
    // calc max size for each level
    SizeVector maxLevelSize(tocLevels_, 0);
    {
        UnitArray::iterator it = units_.begin(), it_end = units_.end();
        for(; it < it_end; ++it)
        {
#if defined(_DEBUG)
            if(it->level_ >= tocLevels_)
                InternalError(__FILE__, __LINE__, "incorrect level");
#endif
            if(maxLevelSize[it->level_] < it->size_)
                maxLevelSize[it->level_] = it->size_;
        }
    }

    for(int i = maxLevelSize.size(); --i >= 0;)
        if(maxLevelSize[i] > THRESHOLD_SIZE)
            return i;
    return 0;
}

//-----------------------------------------------------------------------
static String MakeFileName(const String &prefix, int idx)
{
    std::ostringstream fileName;
    fileName << prefix;
    fileName.width(4);
    fileName.fill('0');
    fileName << idx;
    return fileName.str();
}

//-----------------------------------------------------------------------
String ConverterPass2::MakeUniqueId(bool anchor)
{
    std::ostringstream fileId;
    fileId << (anchor ? "anchor" : "id") << uniqueIdIdx_++;
    return fileId.str();
}

//-----------------------------------------------------------------------
void ConverterPass2::BuiltFileLayout(int levelToSplit)
{
    // find cover page
    UnitArray::iterator it = units_.begin(), it_end = units_.end();
    for(; it < it_end; ++it)
        switch(it->type_)
        {
        case Unit::COVERPAGE:   coverPgIt_ = it; break;
        //case Unit::IMAGE:       coverPgIt_ = it; break;
        case Unit::SECTION:     break;
        default:                continue;
        }

    // build layout
    int fileIdx = 0;
    String file;
    int prevLevel = -1;
    Unit::Type prevType = Unit::UNIT_NONE;
    for(it = units_.begin(); it < it_end; ++it)
    {
#if FB2TOEPUB_TOC_REFERS_FILES_ONLY
        if ((it->type_ != prevType && (prevType != Unit::TITLE || it->type_ != Unit::SECTION)) || it->level_ <= levelToSplit)
#else
        if ((it->type_ != prevType && (prevType != Unit::TITLE || it->type_ != Unit::SECTION)) ||
            (it->level_ <= levelToSplit && prevLevel >= levelToSplit) ||
            (it->level_ <= prevLevel && prevLevel <= levelToSplit))
#endif
        {
            if(it == coverPgIt_)
                file = "cover";
            else
                file = MakeFileName("txt", fileIdx++);
        }

#if FB2TOEPUB_TOC_REFERS_FILES_ONLY
        // force exclusion from TOC for all units above split level
        if(it->level_ > levelToSplit)
            it->title_.clear();
#endif

        // assing unit file name
        it->file_ = file;

#if !FB2TOEPUB_TOC_REFERS_FILES_ONLY
        // make and assing new unit id
        it->fileId_ = MakeUniqueId();
#endif

        prevLevel = it->level_;
        prevType = it->type_;
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::BuildOutputLayout()
{
    // adjust sizes
    AdjustUnitSizes();

    // calculate # of toc levels
    CalcTocLevels();

    // determine which level will be splitted to different files, and build file layout
    BuiltFileLayout(CalcLevelToSplit());
}

//-----------------------------------------------------------------------
void ConverterPass2::BuildReferenceMaps(std::set<String> *noteRefIds)
{
    ReferenceMap::iterator refidToNew_end = refidToNew_.end();
    RefidInfoMap::iterator refidToUnit_end = refidToUnit_.end();

    UnitArray::const_iterator cit = units_.begin(), cit_end = units_.end();
    for(; cit < cit_end; ++cit)
    {
        strvector::const_iterator cit1 = cit->refIds_.begin(), cit1_end = cit->refIds_.end();
        for(; cit1 < cit1_end; ++cit1)
        {
            const String &id = *cit1;

            // map original id to new id
            String newId;
            {
                ReferenceMap::iterator it = refidToNew_.lower_bound(id);
                if(it != refidToNew_end && it->first == id)
                    newId = it->second;
                else
                {
                    newId = MakeUniqueId();
                    refidToNew_.insert(it, ReferenceMap::value_type(id, newId));
                }
            }

            // map new unique id to unit
            {
                // The input unit array should not contatin duplicate ids.
                // Actually we are supposed to skip all dup refs in AddId method, pass 1.
                // So if we found one then it is internal error.
                RefidInfoMap::iterator it = refidToUnit_.lower_bound(newId);
#if defined(_DEBUG)
                if(it != refidToUnit_end && it->first == id)
                    InternalError(__FILE__, __LINE__, "duplicate reference id");
#endif
                refidToUnit_.insert(it, RefidInfoMap::value_type(newId, &*cit));
            }
        }

        // store note id
        if(!cit->noteRefId_.empty())
            noteRefIds->insert(cit->noteRefId_);
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::BuildAnchors(const std::set<String> &noteRefIds)
{
    UnitArray::const_iterator cit = units_.begin(), cit_end = units_.end();
    for(; cit < cit_end; ++cit)
    {
        std::set<String>::const_iterator cit1 = cit->refs_.begin(), cit1_end = cit->refs_.end();
        for(; cit1 != cit1_end; ++cit1)
            if(noteRefIds.find(*cit1) != noteRefIds.end())
            {
                // this is id to note/comment section
                String id = refidToNew_[*cit1];

                // make sure that this id appears first time
                ReferenceMap::iterator it = noteidToAnchorId_.lower_bound(id);
                if(it != noteidToAnchorId_.end() && it->first == id)
                    continue;   // already has anchor

                // create new unique anchor id
                String anchorid = MakeUniqueId(true);
                refidToUnit_[anchorid] = &*cit;
                noteidToAnchorId_.insert(it, ReferenceMap::value_type(id, anchorid));
            }
    }
}

//-----------------------------------------------------------------------
String ConverterPass2::Findhref(const AttrMap &attrmap) const
{
    std::set<String>::const_iterator cit = xlns_.begin(), cit_end = xlns_.end();
    for(; cit != cit_end; ++cit)
    {
        String href;
        if(cit->empty())
            href = "href";
        else
            href = (*cit)+":href";
        AttrMap::const_iterator ait = attrmap.find(href);
        if(ait != attrmap.end())
            return ait->second;
    }
    return "";
}

//-----------------------------------------------------------------------
void ConverterPass2::StartUnit(Unit::Type unitType, AttrMap *attrmap)
{
    // close previous section
    if(unitActive_)
    {
        if(unitHasId_)
            pout_->WriteFmt("</div>\n");        // <div id=...> - original id
#if !FB2TOEPUB_TOC_REFERS_FILES_ONLY
        pout_->WriteFmt("</div>\n");            // <div id=...> - file id
#endif
        if(units_[unitIdx_].type_ == Unit::SECTION)
            pout_->WriteFmt("</div>\n");    // <div class="section...>
        ++unitIdx_;
    }

    const Unit &unit = units_[unitIdx_];
    if(prevUnitFile_ != unit.file_)
    {
        prevUnitFile_ = unit.file_;

        // close previous file
        if(unitActive_)
        {
            if(units_[unitIdx_-1].bodyType_ != Unit::BODY_NONE)
                pout_->WriteFmt("</div>\n");    // <div class="body...>
            pout_->WriteFmt("</body>\n");
            pout_->WriteFmt("</html>\n");
        }

        // begin new file
        pout_->BeginFile((String("OPS/") + unit.file_ + ".xhtml").c_str(), true);
        pout_->WriteFmt("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        pout_->WriteFmt("<html xmlns=\"http://www.w3.org/1999/xhtml\">\n");
        pout_->WriteFmt("<head>\n");
        pout_->WriteFmt("<title/>\n");

        strvector::const_iterator cit = cssfiles_.begin(), cit_end = cssfiles_.end();
        for(; cit < cit_end; ++cit)
            pout_->WriteFmt("<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\"/>\n", EncodeStr(*cit).c_str());

        pout_->WriteFmt("</head>\n");
        if(!bodyXmlLang_.empty())
            pout_->WriteFmt("<body xml:lang=\"%s\">\n", EncodeStr(bodyXmlLang_).c_str());
        else
            pout_->WriteFmt("<body>\n");

        switch(unit.bodyType_)
        {
        case Unit::MAIN:        pout_->WriteStr("<div class=\"body_main\">"); break;
        case Unit::NOTES:       pout_->WriteStr("<div class=\"body_notes\">"); break;
        case Unit::COMMENTS:    pout_->WriteStr("<div class=\"body_comments\">"); break;
        case Unit::BODY_NONE:   break;
        default:                InternalError(__FILE__, __LINE__, "StartUnit error");
        }
    }
    if(unit.type_ == Unit::SECTION)
    {
        if(!sectXmlLang_.empty())
            pout_->WriteFmt("<div class=\"section%d\" xml:lang=\"%s\">\n", unit.level_+1, EncodeStr(sectXmlLang_).c_str());
        else
            pout_->WriteFmt("<div class=\"section%d\">\n", unit.level_+1);
    }
#if !FB2TOEPUB_TOC_REFERS_FILES_ONLY
    pout_->WriteFmt("<div id=\"%s\">\n", unit.fileId_.c_str()); // file id
#endif

    unitHasId_ = false;
    if(attrmap)
    {
        String id = (*attrmap)["id"];
        if(!id.empty())
        {
            unitHasId_ = true;
            pout_->WriteFmt("<div id=\"%s\">\n", refidToNew_[id].c_str()); // original id (remapped)
        }
    }
    unitActive_ = true;
}

//-----------------------------------------------------------------------
void ConverterPass2::EndUnit()
{
    if(unitActive_)
    {
        // close last section and file
        if(unitHasId_)
            pout_->WriteFmt("</div>\n");    // <div id=...> - original id
#if !FB2TOEPUB_TOC_REFERS_FILES_ONLY
        pout_->WriteFmt("</div>\n");        // <div id=...> - file id
#endif
        if(units_[unitIdx_].type_ == Unit::SECTION)
            pout_->WriteFmt("</div>\n");    // <div class="section...">
        if(units_[unitIdx_].bodyType_ != Unit::BODY_NONE)
            pout_->WriteFmt("</div>\n");    // <div class="body...">
        pout_->WriteFmt("</body>\n");
        pout_->WriteFmt("</html>\n");

        unitActive_ = false;
        ++unitIdx_;
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::SwitchUnitIfSizeAbove(std::size_t size)
{
    if(sectionSize_ > size)
    {
        sectionSize_ = 0;
        StartUnit(Unit::SECTION);
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::AddMimetype()
{
    static const char contents[] = "application/epub+zip";
    pout_->BeginFile("mimetype", false);
    pout_->Write(contents, sizeof(contents)/sizeof(char)-1);
}

//-----------------------------------------------------------------------
void ConverterPass2::AddContainer()
{
    static const char contents[] =  "<?xml version=\"1.0\"?>\n"
                                    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                                    "  <rootfiles>\n"
                                    "    <rootfile full-path=\"OPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
                                    "  </rootfiles>\n"
                                    "</container>";

    pout_->BeginFile("META-INF/container.xml", true);
    pout_->Write(contents, sizeof(contents)/sizeof(char)-1);
}

//-----------------------------------------------------------------------
void ConverterPass2::AddStyles()
{
    strvector::const_iterator cit = css_.begin(), cit_end = css_.end();
    for(; cit < cit_end; ++cit)
    {
        Ptr<ScanDir> sd = CreateScanDir(cit->c_str(), "css");
        String fname;
        for(String ospath = sd->GetNextFile(&fname); !ospath.empty(); ospath = sd->GetNextFile(&fname))
        {
            fname = String("css/") + fname;
            pout_->AddFile(CreateInFileStm(ospath.c_str()), (String("OPS/") + fname).c_str(), true);
            cssfiles_.push_back(fname);
        }
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::ScanFonts(const char *ext, ExtFileVector *fontfiles)
{
    strvector::const_iterator cit = fonts_.begin(), cit_end = fonts_.end();
    for(; cit < cit_end; ++cit)
    {
        Ptr<ScanDir> sd = CreateScanDir(cit->c_str(), ext);
        String fname;
        for(String ospath = sd->GetNextFile(&fname); !ospath.empty(); ospath = sd->GetNextFile(&fname))
            fontfiles->push_back(ExtFile(String("fonts/") + fname, ospath));
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::AddFontFiles(const ExtFileVector &fontfiles)
{
    ExtFileVector::const_iterator cit = fontfiles.begin(), cit_end = fontfiles.end();
    for(; cit < cit_end; ++cit)
        if(!IsFontEmbedAllowed(cit->ospath_))
            FontError(cit->ospath_, "embedding not allowed");

    for(cit = fontfiles.begin(); cit < cit_end; ++cit)
    {
        // mangle (mangling == deflating + XORing), then store without compression
        Ptr<InStm> stm = CreateManglingStm(CreateInFileStm(cit->ospath_.c_str()), adobeKey_, sizeof(adobeKey_), 1024);
        pout_->AddFile(stm, (String("OPS/") + cit->fname_).c_str(), false);

        // just compress
        //Ptr<InStm> stm = CreateInFileStm(cit->ospath_.c_str());
        //pout_->AddFile(stm, (String("OPS/") + cit->fname_).c_str(), true);
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::MakeCoverPageFirst()
{
    if(coverPgIt_ != units_.end())
    {
        // move cover to begin
        Unit coverUnit = *coverPgIt_;
        units_.erase(coverPgIt_);
        units_.insert(units_.begin(), coverUnit);
        coverPgIt_ = units_.begin();
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::AddContentOpf()
{
    strvector files;
    {
        // build file array
        String prevFile;
        UnitArray::const_iterator cit = units_.begin(), cit_end = units_.end();
        for(; cit < cit_end; ++cit)
            if(prevFile != cit->file_)
            {
                prevFile = cit->file_;
                files.push_back(cit->file_);
            }
    }

    pout_->BeginFile("OPS/content.opf", true);

    pout_->WriteStr("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    pout_->WriteStr("<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"dcidid\" version=\"2.0\">\n\n");

    pout_->WriteStr("  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n");
    pout_->WriteStr("    xmlns:dcterms=\"http://purl.org/dc/terms/\"\n");
    pout_->WriteStr("    xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n");
    pout_->WriteStr("    xmlns:opf=\"http://www.idpf.org/2007/opf\">\n");
    pout_->WriteFmt("    <dc:title>%s</dc:title>\n", (xlitConv_ ? xlitConv_->Convert(title_) : title_).c_str());
    pout_->WriteFmt("    <dc:language>%s</dc:language>\n", lang_.c_str());
    pout_->WriteFmt("    <dc:identifier id=\"dcidid\" opf:scheme=\"uuid\">%s</dc:identifier>\n", id_.c_str());
    {
        strvector::const_iterator cit = authors_.begin(), cit_end = authors_.end();
        for(; cit < cit_end; ++cit)
            pout_->WriteFmt("    <dc:creator opf:role=\"aut\">%s</dc:creator>\n", (xlitConv_ ? xlitConv_->Convert(*cit) : *cit).c_str());
    }
    if(!title_info_date_.empty())
        pout_->WriteFmt("    <dc:date>%s</dc:date>\n", title_info_date_.c_str());
    if(!id1_.empty())
        pout_->WriteFmt("    <dc:identifier id=\"dcidid1\" opf:scheme=\"ID\">%s</dc:identifier>\n", id1_.c_str());
    if(!isbn_.empty())
        pout_->WriteFmt("    <dc:identifier id=\"dcidid2\" opf:scheme=\"isbn\">%s</dc:identifier>\n", isbn_.c_str());

    // Add cover image description
    if(coverBinIdx_ >= 0)
        pout_->WriteFmt("    <meta name=\"cover\" content=\"%s\"/>\n", MakeFileName("bin", coverBinIdx_).c_str());

    pout_->WriteStr("  </metadata>\n\n");

    pout_->WriteStr("  <manifest>\n");
    AddContentManifestFile(pout_, "ncx", "toc.ncx", "application/x-dtbncx+xml");

    // describe binary files
    {
        int i = 0;
        binvector::const_iterator cit = binaries_.begin(), cit_end = binaries_.end();
        for(; cit < cit_end; ++cit)
            AddContentManifestFile(pout_, MakeFileName("bin", i++).c_str(), cit->file_.c_str(), cit->type_.c_str());
    }

    // describe fonts
    {
        int i;
        ExtFileVector::const_iterator cit, cit_end;

        for(cit = ttffiles_.begin(), cit_end = ttffiles_.end(), i = 0; cit < cit_end; ++cit)
            AddContentManifestFile(pout_, MakeFileName("ttf", i++).c_str(), cit->fname_.c_str(), "application/vnd.ms-opentype");

        for(cit = otffiles_.begin(), cit_end = otffiles_.end(), i = 0; cit < cit_end; ++cit)
            AddContentManifestFile(pout_, MakeFileName("otf", i++).c_str(), cit->fname_.c_str(), "application/vnd.ms-opentype");
    }

    // describe stylesheets, manifest-only-fonts, text files
    {
        int i;
        strvector::const_iterator cit, cit_end;

        for(cit = cssfiles_.begin(), cit_end = cssfiles_.end(), i = 0; cit < cit_end; ++cit)
            AddContentManifestFile(pout_, MakeFileName("css", i++).c_str(), cit->c_str(), "text/css");

        for(cit = mfonts_.begin(), cit_end = mfonts_.end(), i = 0; cit < cit_end; ++cit)
            AddContentManifestFile(pout_, MakeFileName("mttf", i++).c_str(), cit->c_str(), "application/vnd.ms-opentype");

        for(cit = files.begin(), cit_end = files.end(); cit < cit_end; ++cit)
            AddContentManifestFile(pout_, cit->c_str(), (*cit + ".xhtml").c_str(), "application/xhtml+xml");
    }
    pout_->WriteStr("  </manifest>\n\n");

    pout_->WriteStr("  <spine toc=\"ncx\">\n");
    {
        strvector::const_iterator cit = files.begin(), cit_end = files.end();
        for(; cit < cit_end; ++cit)
            pout_->WriteFmt("    <itemref idref=\"%s\"/>\n", cit->c_str());
    }
    pout_->WriteStr("  </spine>\n\n");
    pout_->WriteStr("</package>\n");
}

//-----------------------------------------------------------------------
void ConverterPass2::AddTocNcx()
{
    pout_->BeginFile("OPS/toc.ncx", true);
    
    pout_->WriteStr("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    pout_->WriteStr("<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n");

    pout_->WriteStr("<head>\n");
    pout_->WriteFmt("  <meta name=\"dtb:uid\" content=\"%s\"/>\n", id_.c_str());
    pout_->WriteFmt("  <meta name=\"dtb:depth\" content=\"%d\"/>\n", tocLevels_);
    pout_->WriteStr("  <meta name=\"dtb:totalPageCount\" content=\"0\"/>\n");
    pout_->WriteStr("  <meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n");
    pout_->WriteStr("</head>\n");
    pout_->WriteStr("<docTitle>\n");
    pout_->WriteFmt("  <text>%s</text>\n", (xlitConv_ ? xlitConv_->Convert(title_) : title_).c_str());
    pout_->WriteStr("</docTitle>\n");
    pout_->WriteStr("<navMap>\n");

    int navPoint = 1;
    int level = 0;
    bool first = true;
    {
        UnitArray::const_iterator cit = units_.begin(), cit_end = units_.end();
        for(; cit < cit_end; ++cit)
        {
            if(cit->title_.empty())
                continue;

            if (level > cit->level_)
            {
                // close previous
                for(int i = level - cit->level_; --i >= 0;)
                    pout_->WriteFmt("</navPoint>\n");
                pout_->WriteFmt("</navPoint>\n");
            }
            else if(level == cit->level_)
            {
                // close previous
                if(!first)
                    pout_->WriteFmt("</navPoint>\n");
                first = false;
            }
            pout_->WriteFmt("<navPoint id=\"navPoint-%d\" playOrder=\"%d\">\n", navPoint, navPoint);
            pout_->WriteFmt("<navLabel><text>%s</text></navLabel>", (xlitConv_ ? xlitConv_->Convert(cit->title_) : cit->title_).c_str());

#if FB2TOEPUB_TOC_REFERS_FILES_ONLY
            String fullId = cit->file_ + ".xhtml";
#else
            String fullId = cit->file_ + ".xhtml#" + cit->fileId_;
#endif
            pout_->WriteFmt("<content src=\"%s\"/>\n", fullId.c_str());

            level = cit->level_;
            ++navPoint;
        }
        while(--level >= 0)
            pout_->WriteFmt("</navPoint>\n");
        if(!first)
            pout_->WriteFmt("</navPoint>\n");
    }

    pout_->WriteStr("  </navMap>\n");
    pout_->WriteStr("</ncx>\n");
}

//-----------------------------------------------------------------------
void ConverterPass2::AddEncryption()
{
    if(ttffiles_.empty() && otffiles_.empty())
        return;

    pout_->BeginFile("META-INF/encryption.xml", true);
    pout_->WriteStr("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    pout_->WriteStr("<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n");

    {
        int i;
        ExtFileVector::const_iterator cit, cit_end;

        for(cit = ttffiles_.begin(), cit_end = ttffiles_.end(), i = 0; cit < cit_end; ++cit)
        {
            pout_->WriteStr("<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">\n");
            pout_->WriteStr("<EncryptionMethod Algorithm=\"http://ns.adobe.com/pdf/enc#RC\"/>\n");
            pout_->WriteStr("<CipherData>\n");
            pout_->WriteFmt("<CipherReference URI=\"OPS/%s\"/>\n", cit->fname_.c_str());
            pout_->WriteStr("</CipherData>\n");
            pout_->WriteStr("</EncryptedData>\n");
        }
        //AddContentManifestFile(pout_, MakeFileName("ttf", i++).c_str(), cit->c_str(), "application/x-font-ttf");

        for(cit = otffiles_.begin(), cit_end = otffiles_.end(), i = 0; cit < cit_end; ++cit)
        {
            pout_->WriteStr("<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">\n");
            pout_->WriteStr("<EncryptionMethod Algorithm=\"http://ns.adobe.com/pdf/enc#RC\"/>\n");
            pout_->WriteStr("<CipherData>\n");
            pout_->WriteFmt("<CipherReference URI=\"OPS/%s\"/>\n", cit->fname_.c_str());
            pout_->WriteStr("</CipherData>\n");
            pout_->WriteStr("</EncryptedData>\n");
        }
    }

    pout_->WriteStr("</encryption>\n");
}

//-----------------------------------------------------------------------
const String* ConverterPass2::AddId(const AttrMap &attrmap)
{
    AttrMap::const_iterator cit = attrmap.find("id");
    if(cit == attrmap.end())
        return NULL;

    if(allRefIds_.find(cit->second) != allRefIds_.end())
        return NULL;    // ignore second instance

    String id = cit->second;

    // remap it to our new id
    id = refidToNew_[id];
    if(id.empty())
        InternalError(__FILE__, __LINE__, "AddId error");

    pout_->WriteFmt(" id=\"%s\"", EncodeStr(id).c_str());
    return &cit->second;
}

//-----------------------------------------------------------------------
void ConverterPass2::ParseTextAndEndElement (const String &element)
{
    SetScannerDataMode setDataMode(s_);
    for(;;)
    {
        LexScanner::Token t = s_->LookAhead();
        switch(t.type_)
        {
        default:
            s_->EndElement();
            return;

        case LexScanner::DATA:
            sectionSize_ += t.size_;
            pout_->WriteStr(s_->GetToken().s_.c_str());
            continue;

        case LexScanner::START:
            //<strong>, <emphasis>, <stile>, <a>, <strikethrough>, <sub>, <sup>, <code>, <image>
            if(!t.s_.compare("strong"))
                strong();
            else if(!t.s_.compare("emphasis"))
                emphasis();
            else if(!t.s_.compare("style"))
                style();
            else if(!t.s_.compare("a"))
                a();
            else if(!t.s_.compare("strikethrough"))
                strikethrough();
            else if(!t.s_.compare("sub"))
                sub();
            else if(!t.s_.compare("sup"))
                sup();
            else if(!t.s_.compare("code"))
                code();
            else if(!t.s_.compare("image"))
                image(true, true, false);
            else
            {
                std::ostringstream ss;
                ss << "<" << t.s_ << "> unexpected in <" << element + ">";
                s_->Error(ss.str());
            }
            continue;
            //</strong>, </emphasis>, </stile>, </a>, </strikethrough>, </sub>, </sup>, </code>, </image>
        }
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::CopyAttribute(const String &attr, const AttrMap &attrmap)
{
    AttrMap::const_iterator cit = attrmap.find(attr);
    if(cit != attrmap.end())
        pout_->WriteFmt(" %s=\"%s\"", attr.c_str(), EncodeStr(cit->second).c_str());
}

//-----------------------------------------------------------------------
void ConverterPass2::CopyXmlLang(const AttrMap &attrmap)
{
    CopyAttribute("xml:lang", attrmap);
}

//-----------------------------------------------------------------------
void ConverterPass2::FictionBook()
{
    AttrMap attrmap;
    s_->BeginNotEmptyElement("FictionBook", &attrmap);

    // namespaces
    AttrMap::const_iterator cit = attrmap.begin(), cit_end = attrmap.end();
    bool has_fb = false, has_emptyfb = false;
    for(; cit != cit_end; ++cit)
    {
        static const String xmlns = "xmlns";
        static const std::size_t xmlns_len = xmlns.length();
        static const String fbID = "http://www.gribuser.ru/xml/fictionbook/2.0", xlID = "http://www.w3.org/1999/xlink";

        if(!cit->second.compare(fbID))
        {
            if(!cit->first.compare(xmlns))
                has_emptyfb = true;
            else if(cit->first.compare(0, xmlns_len+1, xmlns+":"))
                s_->Error("bad FictionBook namespace definition");
            has_fb = true;
        }
        else if(!cit->second.compare(xlID))
        {
            if(cit->first.compare(0, xmlns_len+1, xmlns+":"))
                s_->Error("bad xlink namespace definition");
            xlns_.insert(cit->first.substr(xmlns_len+1));
        }
    }
    if(!has_fb)
        s_->Error("missing FictionBook namespace definition");
    if(!has_emptyfb)
        s_->Error("non-empty FictionBook namespace not implemented");

    //<stylesheet>
    s_->SkipAll("stylesheet");
    //</stylesheet>

    //<description>
    description();
    //</description>

    //<body>
    body();
    if(s_->IsNextElement("body"))
        body();
    if(s_->IsNextElement("body"))
        body();
    //</body>

    //<binary>
    while(s_->IsNextElement("binary"))
        binary();
    //</binary>

    s_->SkipRestOfElementContent(); // skip rest of <FictionBook>
}

//-----------------------------------------------------------------------
bool ConverterPass2::AddAnchorid(const String &anchorid)
{
    std::set<String>::iterator it = usedAnchorsids_.lower_bound(anchorid);
    if(it != usedAnchorsids_.end() && *it == anchorid)
        return false;   // already processed

    usedAnchorsids_.insert(it, anchorid);
    return true;
}

//-----------------------------------------------------------------------
void ConverterPass2::a()
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("a", &attrmap);

    String id = Findhref(attrmap);
    if(id.empty())
        s_->Error("<a> should have href attribute");

    bool anchorSet = false;
    if(id[0] != '#')
    {
        // external reference
        pout_->WriteFmt("<a class=\"e_a\" href=\"%s\"", EncodeStr(id).c_str());
        if(!notempty)
        {
            pout_->WriteStr("/>");
            return;
        }
    }
    else
    {
        // internal reference
        id = id.substr(1);
        String file = refidToUnit_[refidToNew_[id]]->file_;

        // remap it to our new id
        id = refidToNew_[id];
        if(id.empty())
            InternalError(__FILE__, __LINE__, "a() error");

        String anchorid = noteidToAnchorId_[id];
        if(!anchorid.empty() && AddAnchorid(anchorid))
        {
            anchorSet = true;
            pout_->WriteFmt("<span id=\"%s\">", anchorid.c_str());
        }

        pout_->WriteFmt("<a href=\"%s.xhtml#%s\"", file.c_str(), id.c_str());
        if(!notempty)
        {
            pout_->WriteStr("/>");
            if(anchorSet)
                pout_->WriteFmt("</span>");
            return;
        }
    }
    pout_->WriteStr(">");

    SetScannerDataMode setDataMode(s_);
    for(;;)
    {
        LexScanner::Token t = s_->LookAhead();
        switch(t.type_)
        {
        default:
            s_->EndElement();
            pout_->WriteStr("</a>");
            if(anchorSet)
                pout_->WriteFmt("</span>");
            return;

        case LexScanner::DATA:
            sectionSize_ += t.size_;
            pout_->WriteStr(s_->GetToken().s_.c_str());
            continue;

        case LexScanner::START:
            //<strong>, <emphasis>, <stile>, <strikethrough>, <sub>, <sup>, <code>, <image>
            if(!t.s_.compare("strong"))
                strong();
            else if(!t.s_.compare("emphasis"))
                emphasis();
            else if(!t.s_.compare("style"))
                style();
            else if(!t.s_.compare("strikethrough"))
                strikethrough();
            else if(!t.s_.compare("sub"))
                sub();
            else if(!t.s_.compare("sup"))
                sup();
            else if(!t.s_.compare("code"))
                code();
            else if(!t.s_.compare("image"))
                image(true, true, false);
            else
            {
                std::ostringstream ss;
                ss << "<" << t.s_ << "> unexpected in <a>";
                s_->Error(ss.str());
            }
            continue;
            //</strong>, </emphasis>, </stile>, </strikethrough>, </sub>, </sup>, </code>, </image>
        }
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::annotation(bool startUnit)
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("annotation", &attrmap);
    if(startUnit)
        StartUnit(Unit::ANNOTATION);

    pout_->WriteStr("<div class=\"annotation\"");
    AddId(attrmap);
    CopyXmlLang(attrmap);
    if(!notempty)
    {
        pout_->WriteStr("/>");
        return;
    }
    pout_->WriteStr(">");

    for(LexScanner::Token t = s_->LookAhead(); t.type_ == LexScanner::START; t = s_->LookAhead())
    {
        //<p>, <poem>, <cite>, <subtitle>, <empty-line>, <table>
        if(!t.s_.compare("p"))
            p();
        else if(!t.s_.compare("poem"))
            poem();
        else if(!t.s_.compare("cite"))
            cite();
        else if(!t.s_.compare("subtitle"))
            subtitle();
        else if(!t.s_.compare("empty-line"))
            empty_line();
        else if(!t.s_.compare("table"))
            table();
        else
        {
            std::ostringstream ss;
            ss << "<" << t.s_ << "> unexpected in <annotation>";
            s_->Error(ss.str());
        }
        //</p>, </poem>, </cite>, </subtitle>, </empty-line>, </table>
    }

    pout_->WriteStr("</div>\n");
    s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::author()
{
    s_->BeginNotEmptyElement("author");

    String author;
    if(s_->IsNextElement("first-name"))
    {
        author = s_->SimpleTextElement("first-name");

        if(s_->IsNextElement("middle-name"))
            author = Concat(author, " ", s_->SimpleTextElement("middle-name"));

        author = Concat(author, " ", s_->SimpleTextElement("last-name"));
    }
    else if(s_->IsNextElement("nickname"))
        author = s_->SimpleTextElement("nickname");
    else
        s_->Error("<first-name> or <nickname> expected");

    authors_.push_back(author);
    s_->SkipRestOfElementContent();
}

//-----------------------------------------------------------------------
void ConverterPass2::binary()
{
    AttrMap attrmap;
    s_->BeginNotEmptyElement("binary", &attrmap);

    // store binary attributes
    Binary b(attrmap["id"], attrmap["content-type"]);
    //if(b.file_.empty() || (b.type_ != "image/jpeg" && b.type_ != "image/png"))
    if(b.file_.empty() || b.type_.empty())
        s_->Error("invalid <binary> attributes");
    b.file_ = String("bin/") + b.file_;
    binaries_.push_back(b);

    // If it is a cover page image file, remember binary index.
    // It is necessary to add cover image description to metadata
    // section of content.opf
    if(b.file_ == coverFile_ && coverBinIdx_ < 0)
        coverBinIdx_ = binaries_.size()-1;

    // store binary file
    {
        SetScannerDataMode setDataMode(s_);
        LexScanner::Token t = s_->GetToken();
        if(t.type_ != LexScanner::DATA)
            s_->Error("<binary> data expected");

        pout_->BeginFile((String("OPS/") + b.file_).c_str(), false);
        if(!DecodeBase64(t.s_.c_str(), pout_))
            s_->Error("base64 error");
    }

    s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::body()
{
    AttrMap attrmap;
    s_->BeginNotEmptyElement("body", &attrmap);

    // set body language
    SetLanguage l(&bodyXmlLang_, attrmap);

    //<image>
    if(s_->IsNextElement("image"))
    {
        StartUnit(Unit::IMAGE);
        image(false, false, true);
    }
    //</image>

    //<title>
    if(s_->IsNextElement("title"))
        title(true);
    //</title>

    //<epigraph>
    while(s_->IsNextElement("epigraph"))
        epigraph();
    //</epigraph>

    do
    {
        //<section>
        section();
        //</section>
    }
    while(s_->IsNextElement("section"));

    EndUnit();

    s_->SkipRestOfElementContent(); // skip rest of <body>
}

//-----------------------------------------------------------------------
void ConverterPass2::book_title()
{
    title_ = s_->SimpleTextElement("book-title");
}

//-----------------------------------------------------------------------
void ConverterPass2::cite()
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("cite", &attrmap);
    pout_->WriteStr("<div class=\"citation\"");
    AddId(attrmap);
    CopyXmlLang(attrmap);
    if(!notempty)
    {
        pout_->WriteStr("/>");
        return;
    }
    pout_->WriteStr(">");

    for(LexScanner::Token t = s_->LookAhead(); t.type_ == LexScanner::START; t = s_->LookAhead())
    {
        //<p>, <subtitle>, <empty-line>, <poem>, <table>
        if(!t.s_.compare("p"))
            p();
        else if(!t.s_.compare("subtitle"))
            subtitle();
        else if(!t.s_.compare("empty-line"))
            empty_line();
        else if(!t.s_.compare("poem"))
            poem();
        else if(!t.s_.compare("table"))
            table();
        else if(!t.s_.compare("text-author"))
            break;
        else
        {
            std::ostringstream ss;
            ss << "<" << t.s_ << "> unexpected in <cite>";
            s_->Error(ss.str());
        }
        //</p>, </subtitle>, </empty-line>, </poem>, </table>
    }

    //<text-author>
    while(s_->IsNextElement("text-author"))
        text_author();
    //</text-author>

    s_->EndElement();
    pout_->WriteStr("</div>\n");
}

//-----------------------------------------------------------------------
void ConverterPass2::code()
{
    if(s_->BeginElement("code"))
    {
        pout_->WriteStr("<code class=\"e_code\">");
        ParseTextAndEndElement("code");
        pout_->WriteStr("</code>");
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::coverpage()
{
    s_->BeginNotEmptyElement("coverpage");
    StartUnit(Unit::COVERPAGE);
    do
    {
        pout_->WriteStr("<div class=\"coverpage\">");
        image(true, false, true);
        pout_->WriteStr("</div>");
    }
    while(s_->IsNextElement("image"));
    s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::date()
{
    AttrMap attrmap;
    if(s_->BeginElement("date"), &attrmap)
    {
        SetScannerDataMode setDataMode(s_);
        if(s_->LookAhead().type_ == LexScanner::DATA)
        {
            pout_->WriteFmt("<p class=\"date\"");
            CopyXmlLang(attrmap);
            pout_->WriteFmt(">%s</p>\n", s_->GetToken().s_.c_str());
        }
        s_->EndElement();
    }
}

//-----------------------------------------------------------------------
static bool IsDateCorrect(const String &s)
{
    // date format should be YYYY[-MM[-DD]]
    // (but we don't check if year, month or day value is valid!)
    if(s.length() < 4 || !isdigit(s[0]) || !isdigit(s[1]) || !isdigit(s[2]) || !isdigit(s[3]))
        return false;
    if(s.length() > 4 && (s.length() < 7 || s[4] != '-' || !isdigit(s[5]) || !isdigit(s[6])))
        return false;
    if(s.length() > 7 && (s.length() != 10 || s[7] != '-' || !isdigit(s[8]) || !isdigit(s[9])))
        return false;
    return true;
}

//-----------------------------------------------------------------------
String ConverterPass2::date__epub()
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("date", &attrmap);

    String text = attrmap["value"];
    if(IsDateCorrect(text))
    {
        if(notempty)
            s_->EndElement();
        return text;
    }

    if(!notempty)
        return "";

    SetScannerDataMode setDataMode(s_);
    if(s_->LookAhead().type_ == LexScanner::DATA)
        text = s_->GetToken().s_;
    s_->EndElement();
    return IsDateCorrect(text) ? text : String("");
}

//-----------------------------------------------------------------------
void ConverterPass2::description()
{
    s_->BeginNotEmptyElement("description");

    //<title-info>
    title_info();
    //</title-info>

    //<src-title-info>
    s_->SkipIfElement("src-title-info");
    //</src-title-info>

    //<document-info>
    document_info();
    //</document-info>

    //<publish-info>
    if(s_->IsNextElement("publish-info"))
        publish_info();
    //</publish-info>

    s_->SkipRestOfElementContent(); // skip rest of <description>
}

//-----------------------------------------------------------------------
void ConverterPass2::document_info()
{
    s_->BeginNotEmptyElement("document-info");

    //<author>
    s_->CheckAndSkipElement("author");
    s_->SkipAll("author");
    //</author>

    //<program-used>
    s_->SkipIfElement("program-used");
    //</program-used>

    //<date>
    s_->CheckAndSkipElement("date");
    //</date>

    //<src-url>
    s_->SkipAll("src-url");
    //</src-url>

    //<src-ocr>
    s_->SkipIfElement("src-ocr");
    //</src-ocr>

    //<id>
    id();
    //<id>

    s_->SkipRestOfElementContent(); // skip rest of <document-info>
}

//-----------------------------------------------------------------------
void ConverterPass2::emphasis()
{
    if(s_->BeginElement("emphasis"))
    {
        pout_->WriteStr("<em class=\"emphasis\">");
        ParseTextAndEndElement("emphasis");
        pout_->WriteStr("</em>");
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::empty_line()
{
    bool notempty = s_->BeginElement("empty-line");
    pout_->WriteStr("<p class=\"empty-line\"> </p>\n");
    if(notempty)
        s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::epigraph()
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("epigraph", &attrmap);
    pout_->WriteStr("<div class=\"epigraph\"");
    AddId(attrmap);
    if(!notempty)
    {
        pout_->WriteStr("/>");
        return;
    }
    pout_->WriteStr(">");

    for(LexScanner::Token t = s_->LookAhead(); t.type_ == LexScanner::START; t = s_->LookAhead())
    {
        //<p>, <poem>, <cite>, <empty-line>
        if(!t.s_.compare("p"))
            p();
        else if(!t.s_.compare("poem"))
            poem();
        else if(!t.s_.compare("cite"))
            cite();
        else if(!t.s_.compare("empty-line"))
            empty_line();
        else if(!t.s_.compare("text-author"))
            break;
        else
        {
            std::ostringstream ss;
            ss << "<" << t.s_ << "> unexpected in <epigraph>";
            s_->Error(ss.str());
        }
        //</p>, </poem>, </cite>, </empty-line>
    }

    //<text-author>
    while(s_->IsNextElement("text-author"))
        text_author();
    //</text-author>

    s_->EndElement();
    pout_->WriteStr("</div>\n");
}

//-----------------------------------------------------------------------
void ConverterPass2::id()
{
    static const String uuidpfx = "urn:uuid:";

    String id = s_->SimpleTextElement("id"), uuid = id;
    if(!uuid.compare(0, uuidpfx.length(), uuidpfx))
        uuid = uuid.substr(uuidpfx.length());
    if(!IsValidUUID(uuid))
    {
        id1_ = id;
        uuid = GenerateUUID();
    }

    id_ = uuidpfx + uuid;
    MakeAdobeKey(uuid, adobeKey_);
}

//-----------------------------------------------------------------------
void ConverterPass2::image(bool fb2_inline, bool html_inline, bool scale)
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("image", &attrmap);

    // get file href
    String href = Findhref(attrmap), alt = attrmap["alt"];
    if(!href.empty())
    {
        if(href[0] == '#')
        {
            // internal reference
            href = String("bin/") + href.substr(1);

            // remember name of the cover page image file
            if(units_[unitIdx_].type_ == Unit::COVERPAGE && coverFile_.empty())
                coverFile_ = href;
        }

        bool has_id = !fb2_inline && attrmap.find("id") != attrmap.end();
        if(has_id)
        {
            pout_->WriteStr("<div");
            AddId(attrmap);
            pout_->WriteStr(">");
        }

        String group = html_inline ? "span" : "div";

        pout_->WriteFmt("<%s class=\"image\">", group.c_str());
        if(scale)
            pout_->WriteFmt("<img style=\"height: 100%%;\" alt=\"%s\" src=\"%s\"/>", EncodeStr(alt).c_str(), EncodeStr(href).c_str());
        else
            pout_->WriteFmt("<img alt=\"%s\" src=\"%s\"/>", EncodeStr(alt).c_str(), EncodeStr(href).c_str());

        if(!fb2_inline)
        {
            if(html_inline)
                InternalError(__FILE__, __LINE__, "<image> error");
            AttrMap::const_iterator cit = attrmap.find("title");
            if(cit != attrmap.end())
                pout_->WriteFmt("<p>%s</p>\n", EncodeStr(cit->second).c_str());
        }
        pout_->WriteFmt("</%s>", group.c_str());

        if(has_id)
            pout_->WriteStr("</div>\n");
    }
    if(!notempty)
        return;
    ClrScannerDataMode clrDataMode(s_);
    s_->EndElement();
}

//-----------------------------------------------------------------------
String ConverterPass2::isbn()
{
    if(!s_->BeginElement("isbn"))
        return "";

    String text;
    SetScannerDataMode setDataMode(s_);
    if(s_->LookAhead().type_ == LexScanner::DATA)
        text = s_->GetToken().s_;
    s_->EndElement();
    return text;
}

//-----------------------------------------------------------------------
void ConverterPass2::lang()
{
    lang_ = s_->SimpleTextElement("lang");
}

//-----------------------------------------------------------------------
void ConverterPass2::p(const char *pelement, const char *cls)
{
    AttrMap attrmap;
    if(s_->BeginElement("p", &attrmap))
    {
        pout_->WriteFmt("<%s", pelement);
        if(cls)
            pout_->WriteFmt(" class=\"%s\"", cls);
        AddId(attrmap);
        CopyXmlLang(attrmap);
        pout_->WriteStr(">");

        ParseTextAndEndElement("p");
        pout_->WriteFmt("</%s>\n", pelement);
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::poem()
{
    AttrMap attrmap;
    s_->BeginNotEmptyElement("poem", &attrmap);
    pout_->WriteStr("<div class=\"poem\"");
    AddId(attrmap);
    CopyXmlLang(attrmap);
    pout_->WriteStr(">");

    //<title>
    if(s_->IsNextElement("title"))
        title(false);
    //</title>

    //<epigraph>
    while(s_->IsNextElement("epigraph"))
        epigraph();
    //</epigraph>

    //<stanza>
    do
        stanza();
    while(s_->IsNextElement("stanza"));
    //</stanza>

    //<text-author>
    while(s_->IsNextElement("text-author"))
        text_author();
    //</text-author>

    //<data>
    if(s_->IsNextElement("date"))
        date();
    //</data>

    pout_->WriteStr("</div>\n");
    s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::publish_info()
{
    if(!s_->BeginElement("publish-info"))
        return;

    //<book-name>
    s_->SkipIfElement("book-name");
    //</book-name>

    //<publisher>
    s_->SkipIfElement("publisher");
    //</publisher>

    //<city>
    s_->SkipIfElement("city");
    //</city>

    //<year>
    s_->SkipIfElement("year");
    //</year>

    //<isbn>
    if(s_->IsNextElement("isbn"))
        isbn_ = isbn();
    //</isbn>

    s_->SkipRestOfElementContent(); // skip rest of <publish-info>
}

//-----------------------------------------------------------------------
void ConverterPass2::section()
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("section", &attrmap);

    // set section language
    SetLanguage l(&sectXmlLang_, attrmap);

    sectionSize_ = 0;
    StartUnit(Unit::SECTION, &attrmap);

    if(!notempty)
        return;

    //<title>
    if(s_->IsNextElement("title"))
    {
        // add anchor ref
        String id = units_[unitIdx_].noteRefId_;
        if(!id.empty())
        {
            id = noteidToAnchorId_[refidToNew_[id]];
            if(!id.empty())
                id = refidToUnit_[id]->file_ + ".xhtml#" + id;
        }

        title(false, id);
    }
    //</title>

    //<epigraph>
    while(s_->IsNextElement("epigraph"))
        epigraph();
    //</epigraph>

    //<image>
    if(s_->IsNextElement("image"))
        image(false, false, false);
    //</image>

    //<annotation>
    if(s_->IsNextElement("annotation"))
        annotation();
    //</annotation>

    if(s_->IsNextElement("section"))
        do
        {
            //<section>
            section();
            //</section>
        }
        while(s_->IsNextElement("section"));
    else
    {
        for(LexScanner::Token t = s_->LookAhead(); t.type_ == LexScanner::START; t = s_->LookAhead())
        {
            //<p>, <image>, <poem>, <subtitle>, <cite>, <empty-line>, <table>
            if(!t.s_.compare("p"))
                p();
            else if(!t.s_.compare("image"))
            {
                SwitchUnitIfSizeAbove(UNIT_SIZE1);
                image(false, false, false);
            }
            else if(!t.s_.compare("poem"))
            {
                SwitchUnitIfSizeAbove(UNIT_SIZE1);
                poem();
            }
            else if(!t.s_.compare("subtitle"))
            {
                SwitchUnitIfSizeAbove(UNIT_SIZE0);
                subtitle();
            }
            else if(!t.s_.compare("cite"))
            {
                SwitchUnitIfSizeAbove(UNIT_SIZE2);
                cite();
            }
            else if(!t.s_.compare("empty-line"))
            {
                SwitchUnitIfSizeAbove(UNIT_SIZE2);
                empty_line();
            }
            else if(!t.s_.compare("table"))
            {
                SwitchUnitIfSizeAbove(UNIT_SIZE1);
                table();
            }
            else
            {
                std::ostringstream ss;
                ss << "<" << t.s_ << "> unexpected in <section>";
                s_->Error(ss.str());
            }
            //</p>, </image>, </poem>, </subtitle>, </cite>, </empty-line>, </table>

            SwitchUnitIfSizeAbove(MAX_UNIT_SIZE);
        }
    }

    s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::stanza()
{
    s_->BeginNotEmptyElement("stanza");
    pout_->WriteStr("<div class=\"stanza\">");

    //<title>
    if(s_->IsNextElement("title"))
        title(false);
    //</title>

    //<subtitle>
    if(s_->IsNextElement("subtitle"))
        subtitle();
    //</subtitle>

    do
        v();
    while(s_->IsNextElement("v"));

    pout_->WriteStr("</div>\n");
    s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::strikethrough()
{
    if(s_->BeginElement("strikethrough"))
    {
        pout_->WriteStr("<del class=\"strikethrough\">");
        ParseTextAndEndElement("strikethrough");
        pout_->WriteStr("</del>");
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::strong()
{
    if(s_->BeginElement("strong"))
    {
        pout_->WriteStr("<strong class=\"e_strong\">");
        ParseTextAndEndElement("strong");
        pout_->WriteStr("</strong>");
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::style()
{
    // ignore style
    if(s_->BeginElement("style"))
        ParseTextAndEndElement("strong");
}

//-----------------------------------------------------------------------
void ConverterPass2::sub()
{
    if(s_->BeginElement("sub"))
    {
        pout_->WriteStr("<sub class=\"e_sub\">");
        ParseTextAndEndElement("sub");
        pout_->WriteStr("</sub>");
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::subtitle()
{
    AttrMap attrmap;
    if(s_->BeginElement("subtitle", &attrmap))
    {
        pout_->WriteStr("<h2 class=\"e_h2\"");
        AddId(attrmap);
        CopyXmlLang(attrmap);
        pout_->WriteStr(">");

        ParseTextAndEndElement("subtitle");
        pout_->WriteStr("</h2>\n");
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::sup()
{
    if(s_->BeginElement("sup"))
    {
        pout_->WriteStr("<sup class=\"e_sup\">");
        ParseTextAndEndElement("sup");
        pout_->WriteStr("</sup>");
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::table()
{
    AttrMap attrmap;
    s_->BeginNotEmptyElement("table", &attrmap);
    pout_->WriteFmt("<table");
    AddId(attrmap);

    CopyAttribute("style", attrmap);

    pout_->WriteStr(">");
    do
    {
        //<tr>
        tr();
        //</tr>
    }
    while(s_->IsNextElement("tr"));
    pout_->WriteFmt("</table>\n");
    s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::td()
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("td", &attrmap);

    pout_->WriteFmt("<td");
    AddId(attrmap);

    CopyAttribute("style", attrmap);
    CopyAttribute("colspan", attrmap);
    CopyAttribute("rowspan", attrmap);
    CopyAttribute("align", attrmap);
    CopyAttribute("valign", attrmap);
    CopyXmlLang(attrmap);

    if(!notempty)
    {
        pout_->WriteStr("/>");
        return;
    }
    pout_->WriteStr(">");

    ParseTextAndEndElement("td");
    pout_->WriteStr("</td>\n");
}

//-----------------------------------------------------------------------
void ConverterPass2::text_author()
{
    AttrMap attrmap;
    if(s_->BeginElement("text-author", &attrmap))
    {
        pout_->WriteFmt("<div class=\"text_author\"");
        AddId(attrmap);
        CopyXmlLang(attrmap);
        pout_->WriteStr(">");

        ParseTextAndEndElement("text-author");
        pout_->WriteStr("</div>\n");
    }
}

//-----------------------------------------------------------------------
void ConverterPass2::th()
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("th", &attrmap);

    pout_->WriteFmt("<th");
    AddId(attrmap);

    CopyAttribute("style", attrmap);
    CopyAttribute("colspan", attrmap);
    CopyAttribute("rowspan", attrmap);
    CopyAttribute("align", attrmap);
    CopyAttribute("valign", attrmap);
    CopyXmlLang(attrmap);

    if(!notempty)
    {
        pout_->WriteStr("/>");
        return;
    }
    pout_->WriteStr(">");

    ParseTextAndEndElement("th");
    pout_->WriteStr("</th>\n");
}

//-----------------------------------------------------------------------
void ConverterPass2::title(bool startUnit, const String &anchorid)
{
    AttrMap attrmap;
    if(!s_->BeginElement("title", &attrmap))
        return;

    if(startUnit)
        StartUnit(Unit::TITLE);

    pout_->WriteFmt("<div class=\"title\"");
    CopyXmlLang(attrmap);
    pout_->WriteFmt(">\n");
    for(LexScanner::Token t = s_->LookAhead(); t.type_ == LexScanner::START; t = s_->LookAhead())
    {
        if(!t.s_.compare("p"))
        {
            //<p>
            p("h1", "e_h1");
            //</p>
        }
        else if(!t.s_.compare("empty-line"))
        {
            //<empty-line>
            empty_line();
            //</empty-line>
        }
        else
        {
            std::ostringstream ss;
            ss << "<" << t.s_ << "> unexpected in <title>";
            s_->Error(ss.str());
        }
    }
    if(!anchorid.empty())
        pout_->WriteFmt("<h1><span class=\"anchor\"><a href=\"%s\">[&lt;-]</a></span></h1>", anchorid.c_str());
    pout_->WriteStr("</div>\n");

    s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::title_info()
{
    s_->BeginNotEmptyElement("title-info");

    //<genre>
    s_->CheckAndSkipElement("genre");
    s_->SkipAll("genre");
    //</genre>

    //<author>
    do
        author();
    while(s_->IsNextElement("author"));
    //<author>

    //<book-title>
    book_title();
    //</book-title>

    //<annotation>
    if(s_->IsNextElement("annotation"))
        annotation(true);
    //</annotation>

    //<keywords>
    s_->SkipIfElement("keywords");
    //</keywords>

    //<date>
    if(s_->IsNextElement("date"))
        title_info_date_ = date__epub();
    //<date>

    //<coverpage>
    if(s_->IsNextElement("coverpage"))
        coverpage();
    //</coverpage>

    //<lang>
    lang();
    //</lang>

    s_->SkipRestOfElementContent(); // skip rest of <title-info>
}

//-----------------------------------------------------------------------
void ConverterPass2::tr()
{
    AttrMap attrmap;
    bool notempty = s_->BeginElement("tr", &attrmap);
    pout_->WriteStr("<tr");

    CopyAttribute("align", attrmap);

    if(!notempty)
    {
        pout_->WriteStr("/>");
        return;
    }
    pout_->WriteStr(">");

    for(;;)
    {
        //<th>, <td>
        if(s_->IsNextElement("th"))
            th();
        else if(s_->IsNextElement("td"))
            td();
        else
            break;
        //</th>, </td>
    }

    pout_->WriteStr("</tr>\n");
    s_->EndElement();
}

//-----------------------------------------------------------------------
void ConverterPass2::v()
{
    AttrMap attrmap;
    if(s_->BeginElement("v", &attrmap))
    {
        pout_->WriteStr("<p class=\"v\"");
        AddId(attrmap);
        CopyXmlLang(attrmap);
        pout_->WriteStr(">");

        ParseTextAndEndElement("v");
        pout_->WriteStr("</p>\n");
    }
}


void FB2TOEPUB_DECL DoConvertionPass2  (LexScanner *scanner,
                                        const strvector &css,
                                        const strvector &fonts,
                                        const strvector &mfonts,
                                        XlitConv *xlitConv,
                                        UnitArray *units,
                                        OutPackStm *pout)
{
    Ptr<ConverterPass2> conv = new ConverterPass2(scanner, css, fonts, mfonts, xlitConv, units, pout);
    conv->Scan();
}


};  //namespace Fb2ToEpub
