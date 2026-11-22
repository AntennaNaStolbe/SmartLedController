import {identify, light} from 'zigbee-herdsman-converters/lib/modernExtend';

export default {
  zigbeeModel: ['SLCTR-C6-CWW'],
  model: 'SLCTR-C6-CWW',
  vendor: 'AntennaNS',
  description: 'Smart CCT LED controller (Zigbee mesh router)',
  extend: [
    identify(),
    light({colorTemp: {range: [154, 333]}}),
  ],
  ota: true,
};
