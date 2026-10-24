from esphome.components.const import CONF_LABEL
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_TEXT

from ..defines import (
    CONF_LONG_MODE,
    CONF_MAIN,
    CONF_RECOLOR,
    CONF_SCROLLBAR,
    CONF_SELECTED,
    LV_LONG_MODES,
)
from ..lv_validation import lv_bool, lv_text
from ..lvcode import lv, lv_add
from ..schemas import TEXT_SCHEMA
from ..types import LvText
from . import Widget, WidgetType


class LabelType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_LABEL,
            LvText("lv_label_t"),
            (CONF_MAIN, CONF_SCROLLBAR, CONF_SELECTED),
            {
                **TEXT_SCHEMA,
                cv.Optional(CONF_RECOLOR): lv_bool,
                cv.Optional(CONF_LONG_MODE): LV_LONG_MODES.one_of,
            },
        )

    async def to_code(self, w: Widget, config):
        """For a text object, create and set text"""
        if (value := config.get(CONF_TEXT)) is not None:
            from esphome.components import i18n

            key = i18n.register_text(value) if isinstance(value, str) else None
            if key is None:
                await w.set_property(CONF_TEXT, await lv_text.process(value))
            else:
                lv.label_set_text_static(w.obj, cg.RawExpression(i18n.key_expression(key)))
                lv_add(cg.RawExpression(f"esphome::i18n::bind_label({w.obj}, {key})"))
        await w.set_property(CONF_LONG_MODE, config)
        await w.set_property(CONF_RECOLOR, config, processor=lv_bool)


label_spec = LabelType()
