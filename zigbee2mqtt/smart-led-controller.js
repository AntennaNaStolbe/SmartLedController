// SmartLedController CCT router for zigbee2mqtt
// Place this file in a directory referenced by `external_converters` in your
// zigbee2mqtt configuration.yaml, e.g. `data/zigbee2mqtt/external_converters/`.

const {extend} = require('zigbee-herdsman-converters');

const definition = {
    zigbeeModel: ['SLCTR-C6-CWW'],
    model: 'SLCTR-C6-CWW',
    vendor: 'AntennaNS',
    description: 'Smart CCT LED controller (Zigbee mesh router)',
    meta: {},
    extend: [
        extend.light({
            colorTemp: {range: [154, 333]},
            effects: [],
        }),
    ],
    isModernExtend: true,
};

module.exports = definition;