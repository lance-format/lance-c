// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#![allow(dead_code)]

use arrow_array::{
    Array, Int8Array, Int16Array, Int32Array, Int64Array, RecordBatch, StringArray, UInt8Array,
    UInt16Array, UInt32Array, UInt64Array,
};
use arrow_schema::DataType;
use datafusion_substrait::substrait::proto::{
    AggregateFunction, AggregateRel, Expression, FetchRel, FunctionArgument, Plan, PlanRel, Rel,
    RelRoot, Version,
    aggregate_function::AggregationInvocation,
    aggregate_rel::{Grouping, Measure},
    expression::{
        FieldReference, ReferenceSegment, RexType,
        field_reference::{ReferenceType, RootReference, RootType},
        reference_segment::{self, StructField},
    },
    extensions::{
        SimpleExtensionDeclaration, SimpleExtensionUrn,
        simple_extension_declaration::{ExtensionFunction, MappingType},
    },
    function_argument::ArgType,
    rel::RelType,
};
use prost::Message;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AggregateKind {
    CountStar,
    Count(i32),
    Sum(i32),
    Min(i32),
    Max(i32),
}

impl AggregateKind {
    fn name(self) -> &'static str {
        match self {
            Self::CountStar | Self::Count(_) => "count",
            Self::Sum(_) => "sum",
            Self::Min(_) => "min",
            Self::Max(_) => "max",
        }
    }

    fn column(self) -> Option<i32> {
        match self {
            Self::CountStar => None,
            Self::Count(column) | Self::Sum(column) | Self::Min(column) | Self::Max(column) => {
                Some(column)
            }
        }
    }
}

fn field_reference(column: i32) -> Expression {
    Expression {
        rex_type: Some(RexType::Selection(Box::new(FieldReference {
            reference_type: Some(ReferenceType::DirectReference(ReferenceSegment {
                reference_type: Some(reference_segment::ReferenceType::StructField(Box::new(
                    StructField {
                        field: column,
                        child: None,
                    },
                ))),
            })),
            root_type: Some(RootType::RootReference(RootReference {})),
        }))),
    }
}

fn extension(anchor: u32, name: &str) -> SimpleExtensionDeclaration {
    SimpleExtensionDeclaration {
        mapping_type: Some(MappingType::ExtensionFunction(ExtensionFunction {
            extension_urn_reference: 1,
            function_anchor: anchor,
            name: name.to_string(),
        })),
    }
}

fn measure(anchor: u32, aggregate: AggregateKind) -> Measure {
    let arguments = aggregate
        .column()
        .map(|column| FunctionArgument {
            arg_type: Some(ArgType::Value(field_reference(column))),
        })
        .into_iter()
        .collect();
    Measure {
        measure: Some(AggregateFunction {
            function_reference: anchor,
            arguments,
            options: vec![],
            output_type: None,
            phase: 0,
            sorts: vec![],
            invocation: AggregationInvocation::All as i32,
            #[allow(deprecated)]
            args: vec![],
        }),
        filter: None,
    }
}

pub fn aggregate_plan(
    group_by: &[i32],
    aggregates: &[AggregateKind],
    output_names: &[&str],
) -> Vec<u8> {
    let grouping_expressions = group_by.iter().copied().map(field_reference).collect();
    let groupings = if group_by.is_empty() {
        vec![]
    } else {
        vec![Grouping {
            #[allow(deprecated)]
            grouping_expressions: vec![],
            expression_references: (0..group_by.len() as u32).collect(),
        }]
    };
    let extensions = aggregates
        .iter()
        .enumerate()
        .map(|(anchor, aggregate)| extension(anchor as u32, aggregate.name()))
        .collect();
    let measures = aggregates
        .iter()
        .enumerate()
        .map(|(anchor, aggregate)| measure(anchor as u32, *aggregate))
        .collect();
    let aggregate = Rel {
        rel_type: Some(RelType::Aggregate(Box::new(AggregateRel {
            common: None,
            input: None,
            groupings,
            measures,
            grouping_expressions,
            advanced_extension: None,
        }))),
    };
    Plan {
        version: Some(Version {
            major_number: 0,
            minor_number: 63,
            patch_number: 0,
            git_hash: String::new(),
            producer: "lance-c-tests".to_string(),
        }),
        extension_urns: vec![SimpleExtensionUrn {
            extension_urn_anchor: 1,
            urn:
                "https://github.com/substrait-io/substrait/blob/main/extensions/functions_aggregate_generic.yaml"
                    .to_string(),
        }],
        extensions,
        relations: vec![PlanRel {
            rel_type: Some(datafusion_substrait::substrait::proto::plan_rel::RelType::Root(
                RelRoot {
                    input: Some(aggregate),
                    names: output_names.iter().map(|name| (*name).to_string()).collect(),
                },
            )),
        }],
        advanced_extensions: None,
        expected_type_urls: vec![],
        parameter_bindings: vec![],
        type_aliases: vec![],
    }
    .encode_to_vec()
}

pub fn empty_plan() -> Vec<u8> {
    Plan::default().encode_to_vec()
}

pub fn non_aggregate_plan() -> Vec<u8> {
    Plan {
        relations: vec![PlanRel {
            rel_type: Some(
                datafusion_substrait::substrait::proto::plan_rel::RelType::Root(RelRoot {
                    input: Some(Rel {
                        rel_type: Some(RelType::Fetch(Box::new(FetchRel::default()))),
                    }),
                    names: vec![],
                }),
            ),
        }],
        ..Default::default()
    }
    .encode_to_vec()
}

pub fn malformed_plan() -> Vec<u8> {
    vec![0x0a, 0x05]
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Cell {
    Null,
    Int(i64),
    Text(String),
}

pub fn cells(batches: &[RecordBatch]) -> Vec<Vec<Cell>> {
    let mut rows = Vec::new();
    for batch in batches {
        for row in 0..batch.num_rows() {
            let mut values = Vec::with_capacity(batch.num_columns());
            for column in batch.columns() {
                if column.is_null(row) {
                    values.push(Cell::Null);
                    continue;
                }
                let value = match column.data_type() {
                    DataType::Int8 => Cell::Int(
                        column
                            .as_any()
                            .downcast_ref::<Int8Array>()
                            .unwrap()
                            .value(row) as i64,
                    ),
                    DataType::Int16 => Cell::Int(
                        column
                            .as_any()
                            .downcast_ref::<Int16Array>()
                            .unwrap()
                            .value(row) as i64,
                    ),
                    DataType::Int32 => Cell::Int(
                        column
                            .as_any()
                            .downcast_ref::<Int32Array>()
                            .unwrap()
                            .value(row) as i64,
                    ),
                    DataType::Int64 => Cell::Int(
                        column
                            .as_any()
                            .downcast_ref::<Int64Array>()
                            .unwrap()
                            .value(row),
                    ),
                    DataType::UInt8 => Cell::Int(
                        column
                            .as_any()
                            .downcast_ref::<UInt8Array>()
                            .unwrap()
                            .value(row) as i64,
                    ),
                    DataType::UInt16 => Cell::Int(
                        column
                            .as_any()
                            .downcast_ref::<UInt16Array>()
                            .unwrap()
                            .value(row) as i64,
                    ),
                    DataType::UInt32 => Cell::Int(
                        column
                            .as_any()
                            .downcast_ref::<UInt32Array>()
                            .unwrap()
                            .value(row) as i64,
                    ),
                    DataType::UInt64 => Cell::Int(
                        column
                            .as_any()
                            .downcast_ref::<UInt64Array>()
                            .unwrap()
                            .value(row) as i64,
                    ),
                    DataType::Utf8 => Cell::Text(
                        column
                            .as_any()
                            .downcast_ref::<StringArray>()
                            .unwrap()
                            .value(row)
                            .to_string(),
                    ),
                    data_type => panic!("unsupported aggregate result type {data_type}"),
                };
                values.push(value);
            }
            rows.push(values);
        }
    }
    rows
}
