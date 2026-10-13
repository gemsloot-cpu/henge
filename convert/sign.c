/* Ad-hoc code signing for thin arm64 Mach-O files (what `codesign -s -` does).
 *
 * Layout written at the end of __LINKEDIT (16-byte aligned), all fields big-endian:
 *   SuperBlob (0xfade0cc0)
 *     CodeDirectory (0xfade0c02, v0x20400, CS_ADHOC, SHA-256, 4 KiB pages)
 *     Requirements  (0xfade0c01, empty)   -> special slot -2
 *     CMS wrapper   (0xfade0b01, empty)   ad-hoc: no signature
 *
 * Identity (CMS) signing with the user's certificate is not implemented yet.
 */
#include <stdlib.h>
#include <string.h>

#include <CommonCrypto/CommonDigest.h>
#include <mach-o/loader.h>

#include "internal.h"

#define CSMAGIC_EMBEDDED_SIGNATURE 0xfade0cc0u
#define CSMAGIC_CODEDIRECTORY 0xfade0c02u
#define CSMAGIC_REQUIREMENTS 0xfade0c01u
#define CSMAGIC_BLOBWRAPPER 0xfade0b01u
#define CSSLOT_CODEDIRECTORY 0u
#define CSSLOT_REQUIREMENTS 2u
#define CSSLOT_SIGNATURESLOT 0x10000u
#define CS_ADHOC 0x2u
#define CS_HASHTYPE_SHA256 2u
#define CS_EXECSEG_MAIN_BINARY 0x1u
#define CD_VERSION 0x20400u
#define CD_HEADER_SIZE 88u /* CodeDirectory fields up to and including execSegFlags */
#define CS_PAGE_LOG2 12u
#define CS_PAGE (1u << CS_PAGE_LOG2)
#define HASH_SIZE CC_SHA256_DIGEST_LENGTH
#define N_SPECIAL 2u /* slot -1 (Info.plist, unused) and -2 (requirements) */

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void be64(uint8_t *p, uint64_t v) {
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}

int henge_sign_adhoc(const uint8_t *macho, size_t size, const char *identifier, uint8_t **out,
                     size_t *out_size, char *err, size_t errlen) {
    *out = NULL;
    *out_size = 0;
    if (!identifier || !*identifier) return hc_fail(err, errlen, "signing identifier is required");
    if (size < sizeof(struct mach_header_64)) return hc_fail(err, errlen, "file too small for a Mach-O header");
    const struct mach_header_64 *mh = (const struct mach_header_64 *)macho;
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64)
        return hc_fail(err, errlen, "not a thin arm64 Mach-O");
    if (mh->filetype != MH_DYLIB && mh->filetype != MH_EXECUTE && mh->filetype != MH_BUNDLE)
        return hc_fail(err, errlen, "unsupported Mach-O filetype %u", mh->filetype);
    if (mh->sizeofcmds > size - sizeof *mh) return hc_fail(err, errlen, "load commands extend past end of file");

    /* ---- walk load commands ---- */
    const struct segment_command_64 *text = NULL, *linkedit = NULL;
    const struct linkedit_data_command *cs = NULL;
    size_t linkedit_off = 0, cs_off = 0;
    uint64_t first_content = size; /* lowest file offset of section/segment content after the header */
    size_t o = sizeof *mh;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (o + sizeof(struct load_command) > sizeof *mh + mh->sizeofcmds)
            return hc_fail(err, errlen, "truncated load command #%u", i);
        const struct load_command *lc = (const struct load_command *)(macho + o);
        if (lc->cmdsize < sizeof *lc || o + lc->cmdsize > sizeof *mh + mh->sizeofcmds)
            return hc_fail(err, errlen, "bad cmdsize in load command #%u", i);
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const struct segment_command_64 *)lc;
            if (sizeof *s + (uint64_t)s->nsects * sizeof(struct section_64) > lc->cmdsize)
                return hc_fail(err, errlen, "bad section count in %.16s", s->segname);
            if (!strncmp(s->segname, SEG_TEXT, 16)) text = s;
            if (!strncmp(s->segname, SEG_LINKEDIT, 16)) { linkedit = s; linkedit_off = o; }
            if (s->fileoff > 0 && s->filesize > 0 && s->fileoff < first_content) first_content = s->fileoff;
            const struct section_64 *sect = (const struct section_64 *)(s + 1);
            for (uint32_t k = 0; k < s->nsects; k++) {
                uint32_t type = sect[k].flags & SECTION_TYPE;
                if (type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL) continue;
                if (sect[k].offset > 0 && sect[k].size > 0 && sect[k].offset < first_content)
                    first_content = sect[k].offset;
            }
        } else if (lc->cmd == LC_CODE_SIGNATURE) {
            cs = (const struct linkedit_data_command *)lc;
            cs_off = o;
        }
        o += lc->cmdsize;
    }
    if (!text || !linkedit) return hc_fail(err, errlen, "missing __TEXT or __LINKEDIT segment");
    if (linkedit->fileoff > size || linkedit->filesize > size - linkedit->fileoff)
        return hc_fail(err, errlen, "__LINKEDIT extends past end of file");

    /* Bytes covered by the signature end where the old signature started (or at the end of
     * __LINKEDIT when unsigned). */
    uint64_t base = linkedit->fileoff + linkedit->filesize;
    if (cs) {
        if (cs->dataoff < linkedit->fileoff || cs->dataoff > base)
            return hc_fail(err, errlen, "existing code signature is not inside __LINKEDIT");
        base = cs->dataoff;
    }
    if (!cs) {
        uint64_t lc_end = sizeof *mh + (uint64_t)mh->sizeofcmds;
        if (lc_end + sizeof(struct linkedit_data_command) > first_content)
            return hc_fail(err, errlen, "no room for LC_CODE_SIGNATURE after the load commands");
    }

    /* ---- sizes ---- */
    uint64_t dataoff = hc_align_up(base, 16);
    if (dataoff > UINT32_MAX) return hc_fail(err, errlen, "file too large to sign");
    uint32_t ncode = (uint32_t)((dataoff + CS_PAGE - 1) / CS_PAGE);
    size_t ident_len = strlen(identifier) + 1;
    uint32_t ident_offset = CD_HEADER_SIZE;
    uint32_t hash_offset = (uint32_t)(ident_offset + ident_len + N_SPECIAL * HASH_SIZE);
    uint32_t cd_len = hash_offset + ncode * HASH_SIZE;
    const uint32_t req_len = 12, cms_len = 8, sb_hdr = 12 + 3 * 8;
    uint32_t sb_len = sb_hdr + cd_len + req_len + cms_len;
    uint32_t datasize = (uint32_t)hc_align_up(sb_len, 16);

    size_t total = (size_t)(dataoff + datasize);
    uint8_t *mo = calloc(1, total);
    if (!mo) return hc_fail(err, errlen, "out of memory");
    memcpy(mo, macho, (size_t)base);

    /* ---- patch load commands (before hashing: the header is covered by page 0) ---- */
    struct mach_header_64 *omh = (struct mach_header_64 *)mo;
    struct linkedit_data_command *ocs;
    if (cs) {
        ocs = (struct linkedit_data_command *)(mo + cs_off);
    } else {
        ocs = (struct linkedit_data_command *)(mo + sizeof *omh + omh->sizeofcmds);
        ocs->cmd = LC_CODE_SIGNATURE;
        ocs->cmdsize = sizeof *ocs;
        omh->ncmds++;
        omh->sizeofcmds += sizeof *ocs;
    }
    ocs->dataoff = (uint32_t)dataoff;
    ocs->datasize = datasize;
    struct segment_command_64 *ole = (struct segment_command_64 *)(mo + linkedit_off);
    ole->filesize = dataoff + datasize - ole->fileoff;
    if (ole->vmsize < hc_align_up(ole->filesize, HC_PAGE)) ole->vmsize = hc_align_up(ole->filesize, HC_PAGE);

    /* ---- SuperBlob ---- */
    uint8_t *sb = mo + dataoff;
    uint32_t cd_at = sb_hdr, req_at = cd_at + cd_len, cms_at = req_at + req_len;
    be32(sb, CSMAGIC_EMBEDDED_SIGNATURE);
    be32(sb + 4, sb_len);
    be32(sb + 8, 3);
    be32(sb + 12, CSSLOT_CODEDIRECTORY); be32(sb + 16, cd_at);
    be32(sb + 20, CSSLOT_REQUIREMENTS);  be32(sb + 24, req_at);
    be32(sb + 28, CSSLOT_SIGNATURESLOT); be32(sb + 32, cms_at);

    uint8_t *req = sb + req_at;
    be32(req, CSMAGIC_REQUIREMENTS);
    be32(req + 4, req_len);
    be32(req + 8, 0);

    uint8_t *cms = sb + cms_at;
    be32(cms, CSMAGIC_BLOBWRAPPER);
    be32(cms + 4, cms_len);

    /* ---- CodeDirectory ---- */
    uint8_t *cd = sb + cd_at;
    be32(cd + 0, CSMAGIC_CODEDIRECTORY);
    be32(cd + 4, cd_len);
    be32(cd + 8, CD_VERSION);
    be32(cd + 12, CS_ADHOC);
    be32(cd + 16, hash_offset);
    be32(cd + 20, ident_offset);
    be32(cd + 24, N_SPECIAL);
    be32(cd + 28, ncode);
    be32(cd + 32, (uint32_t)dataoff); /* codeLimit */
    cd[36] = HASH_SIZE;
    cd[37] = CS_HASHTYPE_SHA256;
    cd[38] = 0; /* platform */
    cd[39] = CS_PAGE_LOG2;
    /* spare2, scatterOffset, teamOffset, spare3, codeLimit64: 0 */
    be64(cd + 64, text->fileoff);  /* execSegBase */
    be64(cd + 72, text->filesize); /* execSegLimit */
    be64(cd + 80, mh->filetype == MH_EXECUTE ? CS_EXECSEG_MAIN_BINARY : 0);
    memcpy(cd + ident_offset, identifier, ident_len);

    /* Special slot -2 = hash of the requirements blob; slot -1 stays zero. */
    CC_SHA256(req, req_len, cd + hash_offset - CSSLOT_REQUIREMENTS * HASH_SIZE);
    for (uint32_t i = 0; i < ncode; i++) {
        uint64_t start = (uint64_t)i * CS_PAGE;
        uint64_t n = dataoff - start < CS_PAGE ? dataoff - start : CS_PAGE;
        CC_SHA256(mo + start, (CC_LONG)n, cd + hash_offset + i * HASH_SIZE);
    }

    *out = mo;
    *out_size = total;
    return 0;
}
