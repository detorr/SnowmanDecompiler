/* The file is part of Snowman decompiler. */
/* See doc/licenses.asciidoc for the licensing information. */

#include "BfdParser.h"

#include <QCoreApplication> /* For Q_DECLARE_TR_FUNCTIONS. */
#include <QFile>
#include <QDebug>

#include <nc/common/Foreach.h>
#include <nc/common/LogToken.h>
#include <nc/common/Range.h>
#include <nc/common/make_unique.h>

#include <nc/core/image/Image.h>
#include <nc/core/image/Reader.h>
#include <nc/core/image/Relocation.h>
#include <nc/core/image/Section.h>
#include <nc/core/input/ParseError.h>
#include <nc/core/input/Utils.h>

#define PACKAGE 1 /* Work-around for bfd.h */
#define PACKAGE_VERSION 1 /* Work-around for bfd.h */

#include "bfd.h"

namespace nc {
namespace input {
namespace bfdparser {

namespace {

using nc::core::input::read;
using nc::core::input::getAsciizString;
using nc::core::input::ParseError;

class BfdParserImpl {
    Q_DECLARE_TR_FUNCTIONS(BfdParserImpl)

    QIODevice *source_;
    core::image::Image *image_;
    const LogToken &log_;
    bfd *abfd = nullptr;

    ByteOrder byteOrder_;
    std::vector<std::unique_ptr<core::image::Section>> sections_;
    std::vector<const core::image::Symbol *> symbols_;

public:
    BfdParserImpl(QIODevice *source, core::image::Image *image, const LogToken &log):
        source_(source), image_(image), log_(log), byteOrder_(ByteOrder::Current)
    {}

    void parse() {
		QFile *file = static_cast<QFile *>(source_);
		QString filename = file->fileName();
		source_->seek(0); /* Convention */

		/* Read filename */
		if((abfd = bfd_openr(filename.toAscii().data(), nullptr)) == nullptr){
			throw ParseError(tr("Could not open file: %1").arg(filename));
		}
  
  		/* Here we try to figure out the target. */
  		if (!bfd_check_format(abfd, bfd_object)){
  			if (!bfd_check_format(abfd, bfd_archive)){
				bfd_close(abfd);
				throw ParseError(tr("Could not open file: %1").arg(filename));
			} else {
				bfd_close(abfd);
				throw ParseError(tr("BFD archives is not yet supported."));
			}
		}
  
  		int arch = bfd_get_arch(abfd);
  		byteOrder_ = bfd_big_endian(abfd) ? ByteOrder::BigEndian : ByteOrder::LittleEndian;
  	
  		switch (arch) {
            case bfd_arch_i386:
                if(bfd_get_arch_size(abfd) == 32){
	                image_->platform().setArchitecture(QLatin1String("i386"));
                } else {
                	image_->platform().setArchitecture(QLatin1String("x86-64"));
                }
                break;
            case bfd_arch_arm:
                if (byteOrder_ == ByteOrder::LittleEndian) {
                    image_->platform().setArchitecture(QLatin1String("arm-le"));
                } else {
                    image_->platform().setArchitecture(QLatin1String("arm-be"));
                }
                break;
            case bfd_arch_mips:
                if (byteOrder_ == ByteOrder::LittleEndian) {
					if(bfd_get_arch_size(abfd) == 32) {
                        image_->platform().setArchitecture(QLatin1String("mips-le"));
                    } else {
                    	image_->platform().setArchitecture(QLatin1String("mips64-le"));
                    }
                } 
                else if(bfd_get_arch_size(abfd) == 32) {
                    image_->platform().setArchitecture(QLatin1String("mips-be"));
                } else {
                	image_->platform().setArchitecture(QLatin1String("mips64-be"));
                }
                break;
            default:
            	const char *id = bfd_printable_name(abfd);
            	bfd_close(abfd);
                throw ParseError(tr("Unknown machine id: %1.").arg(getAsciizString(id)));
        }

        parseSections();
        parseSymbols();
        parseRelocations();     

        foreach (auto &section, sections_) {
            image_->addSection(std::move(section));
        }

		bfd_close(abfd);
		return;        
    }

private:

    void parseSections() {
    	/* Make a callback to dump_sections_headers() */
    	asection *p;

       	for (p = abfd->sections; p != NULL; p = p->next){
			unsigned int opb = bfd_octets_per_byte(abfd);

		  	/* Ignore linker created section.  See elfNN_ia64_object_p in bfd/elfxx-ia64.c.  */
  			if(p->flags & SEC_LINKER_CREATED){
  				log_.warning(tr("Ignoring linker created section."));
	    		continue;
  			}

			auto section = std::make_unique<core::image::Section>(getAsciizString(bfd_get_section_name(abfd, p)), bfd_get_section_vma(abfd, p), static_cast<unsigned long>(bfd_section_size(abfd, p) / opb));

			section->setAllocated(p->flags & SEC_ALLOC);
			section->setReadable();
			section->setWritable(!(p->flags & SEC_READONLY));
			section->setExecutable(p->flags & SEC_CODE);

			section->setCode(p->flags & SEC_CODE);
			section->setData(p->flags & SEC_DATA);
  			section->setBss((strcmp(bfd_section_name(abfd, p), ".bss") == 0) || (strcmp(bfd_section_name(abfd, p), ".sbss") == 0));
			section->setName(bfd_section_name(abfd, p));

			bfd_size_type strsize = bfd_section_size(abfd, p);
			bfd_byte *content = (bfd_byte *) malloc(strsize);
			
			if (!bfd_get_section_contents(abfd, p, content, 0, strsize)){
				free(content);
				bfd_close(abfd);
				throw ParseError(tr("Could not parse content in sections."));
			}

			QByteArray bytes(reinterpret_cast<const char *>(content));
			section->setContent(std::move(bytes));
			sections_.push_back(std::move(section));
			free(content);
       	}		
		return;
    }


    void parseRelocations() {
    	/* Make a callback to dump_sections_headers() */
    	asection *p;

       	for (p = abfd->sections; p != NULL; p = p->next){
			unsigned int opb = bfd_octets_per_byte(abfd);

		  	/* Ignore linker created section.  See elfNN_ia64_object_p in bfd/elfxx-ia64.c.  */
  			/*if(p->flags & SEC_LINKER_CREATED){
  				log_.warning(tr("Ignoring linker created section."));
	    		continue;
  			}
  			
			if (bfd_is_abs_section(p) || bfd_is_und_section(p) || bfd_is_com_section(p) || ((p->flags & SEC_RELOC) == 0)){
  				continue; 
			}

			arelent **relpp;
			long relcount;
		 	long relsize = bfd_get_reloc_upper_bound (abfd, section);

			if (relsize < 0){
    			bfd_close(abfd);
    			throw ParseError(tr("Could not parse relocations."));
		  	}

			if (relsize == 0){
				log_.warning(tr("Cannot find any relocs."));
				return;
			}

			relpp = (arelent **) malloc(relsize);
			relcount = bfd_canonicalize_reloc(abfd, p, relpp, syms);

			if (relcount < 0){
    	  		free(relpp);
    			bfd_close(abfd);
    			throw ParseError(tr("Failed to read relocations."));
      		}
			
			QString name = getAsciizString(sym_name);
			
			auto relocation = std::make_unique<core::image::Relocation>(entryAddress, image_->addSymbol(std::make_unique<core::image::Symbol>(core::image::SymbolType::FUNCTION, std::move(name), boost::none)));

			image_->addRelocation(std::move(relocation));
			free (relpp);*/
       	}		
		return;
	}


    void parseSymbols() {
		unsigned int symsize;
		long symcount;
		asymbol **syms;
		
		if (!(bfd_get_file_flags(abfd) & HAS_SYMS)){
			log_.warning(tr("Cannot find any symbols."));
    		return;
		}

  		symcount = bfd_read_minisymbols (abfd, FALSE, (void **) &syms, &symsize);
  		if (symcount == 0){
    		symcount = bfd_read_minisymbols (abfd, TRUE /* dynamic */, (void **) &syms, &symsize);
  		}
  	
		if (symcount < 0) {
			bfd_close(abfd);
			throw ParseError(tr("bfd_canonicalize_symtab: %1.").arg(getAsciizString(bfd_errmsg(bfd_get_error()))));
		}

		for (int i = 0; i < symcount; i++) {
			using core::image::Symbol;
			using core::image::SymbolType;
			asymbol *asym = syms[i];
			const char *sym_name = bfd_asymbol_name(asym);
			char symclass = bfd_decode_symclass(asym);
			int sym_value = bfd_asymbol_value(asym);

			QString name = getAsciizString(sym_name);
			boost::optional<ConstantValue> value = static_cast<long>(sym_value);
			const core::image::Section *section = nullptr;
	    	asection *p;
    	   	int j = 0;

    	   	for (p = abfd->sections; p != NULL; p = p->next){
				if(bfd_get_section(asym) == p){
					section = sections_[j].get();
					break;
				}
				j++;
			}
			
			/*
			if(section == nullptr){
				unsigned int opb = bfd_octets_per_byte(abfd);
				auto tmpsection = std::make_unique<core::image::Section>(getAsciizString(bfd_get_section_name(abfd, bfd_get_section(asym))), bfd_get_section_vma(abfd, bfd_get_section(asym)), static_cast<unsigned long>(bfd_section_size(abfd, bfd_get_section(asym)) / opb));
				section = tmpsection.get();
			}
			*/

			//qDebug()  << name << "is:"  << bfd_section_name(abfd, p);
			
            SymbolType type;
			switch (symclass) {
				case 'a':
				case 'D':
				case 'd':
				case 'G':
				case 'g':
				case 'S': /* Small object */
				case 's':
				case 'w':
					type = SymbolType::OBJECT;
					break;
				case 'I':
				case 'i':
				case 'T': /* .text */
				case 't':
				case 'U':
				case 'u':
				case 'V':
				case 'v':
				case 'W':
					type = SymbolType::FUNCTION;
					break;
				case 'A':
				case 'B': /* BSS */
				case 'b':
				case 'R': /* Read-only */
				case 'r':
				case 'N': /* Debug */
				case 'n':
					type = SymbolType::SECTION;
					break;
				default:
					type = SymbolType::NOTYPE;
					break;
			}

            auto sym = std::make_unique<Symbol>(type, name, value, section);
            symbols_.push_back(sym.get());
            image_->addSymbol(std::move(sym));
		}

		free(syms);
		syms = nullptr;
		
		return;
    }
};

} // anonymous namespace

BfdParser::BfdParser():
    core::input::Parser(QLatin1String("BFD"))
{}

bool BfdParser::doCanParse(QIODevice *source) const {
	bfd *abfd = nullptr;
	QFile *file = static_cast<QFile *>(source);
	QString filename = file->fileName();
	
	bfd_init();
	abfd = bfd_openr(filename.toAscii().data(), nullptr);
	if(abfd == nullptr){
		return false;
	}
	if (!(bfd_check_format(abfd, bfd_object) || bfd_check_format(abfd, bfd_archive))){
		bfd_close(abfd);
		return false;
	}
	bfd_close(abfd);
	return true;
}

void BfdParser::doParse(QIODevice *source, core::image::Image *image, const LogToken &log) const {
	BfdParserImpl(source, image, log).parse();
}

} // namespace bfdparser
} // namespace input
} // namespace nc

/* vim:set et sts=4 sw=4: */
