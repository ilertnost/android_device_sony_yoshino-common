#!/vendor/bin/sh

model=`grep -aim1 'model:' /dev/block/by-name/LTALabel | sed -e 's/^.*model:[ ]*\([A-Za-z0-9-]*\).*$/\1/I'` 2> /dev/null

if [ "$model" = "" ]; then
    model=`grep -aEm1 '(701SO|SO-01K|SO-02K|SO-04J|SOV36)&nbsp;' /dev/block/by-name/LTALabel 2>/dev/null | sed -nE 's/.*(701SO|SO-01K|SO-02K|SO-04J|SOV36)&nbsp;.*/\1/p'`
fi

case "$model" in
    "G8142" | "G8342")
        setprop vendor.radio.multisim.config dsds;;
    * )
        setprop vendor.radio.multisim.config ss;;
esac

if [ "$model" == "" ]; then
    setprop vendor.radio.ltalabel.model "unknown"
else
    setprop vendor.radio.ltalabel.model "$model"
fi