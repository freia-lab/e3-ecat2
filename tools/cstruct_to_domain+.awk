#!/usr/bin/awk -f

BEGIN {
    print "static ec_pdo_entry_reg_t domain_regs[] = {"
    in_entries = 0
}

/ec_pdo_entry_info_t/ {
    in_entries = 1
    next
}

in_entries && /^\};/ {
    in_entries = 0
    next
}

in_entries && /^[[:space:]]*\{0x[0-9A-Fa-f]+,/ {

    idx = $1
    subidx = $2
    bitlen = $3

    gsub("\\{", "", idx)
    gsub(",", "", idx)
    gsub(",", "", subidx)
    gsub(",", "", bitlen)

    if (bitlen > 0) {
        name = sprintf("off_%s_%s", substr(idx,3), substr(subidx,3))
        printf("    {0, 0, VENDOR_ID, PRODUCT_CODE, %s, %s, &%s},\n",
               idx, subidx, name)
        offsets[name] = 1
    }
}

END {
    print "    {0}"
    print "};\n"

    print "/* Offsets */"
    for (o in offsets)
        printf("static unsigned int %s;\n", o)
}
