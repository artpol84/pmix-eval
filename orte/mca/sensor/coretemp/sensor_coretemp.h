/*
 * Copyright (c) 2013-2014 Intel, Inc. All rights reserved.
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
/**
 * @file
 *
 * CORETEMP resource manager sensor 
 */
#ifndef ORTE_SENSOR_CORETEMP_H
#define ORTE_SENSOR_CORETEMP_H

#include "orte_config.h"

#include "orte/mca/sensor/sensor.h"

BEGIN_C_DECLS

typedef struct {
    orte_sensor_base_component_t super;
    bool test;
} orte_sensor_coretemp_component_t;

ORTE_MODULE_DECLSPEC extern orte_sensor_coretemp_component_t mca_sensor_coretemp_component;
extern orte_sensor_base_module_t orte_sensor_coretemp_module;


END_C_DECLS

#endif
