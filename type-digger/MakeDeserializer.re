
open Longident;
open Location;

let makeLident = (~moduleName, ~modulePath, ~name) => {
  let base = switch (Str.split(Str.regexp_string("-"), moduleName)) {
    | [one, two] => Ldot(Lident(two), one)
    | [one] => Lident(one)
    | _ => failwith("Bad modulename")
  };
  let base = modulePath->Belt.List.reduce(base, (base, item) => Ldot(base, item));
  Ldot(base, name)
};

let makeTypArgs = variables =>
      variables->Belt.List.mapWithIndex((index, arg) => {
        "arg" ++ string_of_int(index)
      });

let transformerName = (~moduleName, ~modulePath, ~name) =>
  "deserialize_" ++ 
  Str.global_replace(
    Str.regexp_string("-"),
    "__",
    moduleName,
  )  ++ "__" ++ String.concat("__", modulePath) ++ "__" ++ name;

open Parsetree;
open Ast_helper;
open Asttypes;
open SharedTypes.SimpleType;

let makeIdent = lident => Exp.ident(Location.mknoloc(lident));
let ok = v => Exp.construct(mknoloc(Ldot(Ldot(Lident("Belt"), "Result"), "Ok")), Some(v));
let expString = message => Exp.constant(Pconst_string(message, None));
let expError = message => Exp.construct(mknoloc(Lident("Error")), Some(expString(message)));
let expPassError = Exp.construct(mknoloc(Lident("Error")), Some(makeIdent(Lident("error"))));
let patPassError = Pat.construct(mknoloc(Lident("Error")), Some(Pat.var(mknoloc("error"))));
let jsJson = Ldot(Lident("Js"), "Json");

let makeJson = (kind, contents) => Exp.apply(makeIdent(Ldot(jsJson, kind)), [
  (Nolabel, contents)
]);

let makeClassify = makeJson("classify");

let jsonObject = items => makeJson("object_", Exp.apply(
  makeIdent(Ldot(Ldot(Lident("Js"), "Dict"), "fromArray")),
  [(Nolabel, Exp.array(items))]
));

let jsonArray = items => makeJson(
  "array",
  Exp.apply(
    makeIdent(Ldot(Ldot(Lident("Belt"), "List"), "toArray")),
    [(Nolabel, items)]
  )
);

let failer = message => Exp.apply(Exp.ident(Location.mknoloc(Lident("failwith"))), [
  (Nolabel, expString(message))
]);

let sourceTransformer = source => switch source {
  | DigTypes.NotFound => failer("Not found")
  | Public({modulePath, moduleName, name}) =>
    makeIdent(Lident(transformerName(~moduleName, ~modulePath, ~name)))
  | Builtin("list") =>
    [%expr 
      (transformer, list) => switch (Js.Json.classify(list)) {
        | JSONArray(items) =>
          let rec loop = items => switch items {
            | [] => Ok([])
            | [one, ...rest] => switch (transformer(one)) {
              | Belt.Result.Error(error) => Belt.Result.Error(error)
              | Ok(value) => switch (loop(rest)) {
                | Belt.Result.Error(error) => Belt.Result.Error(error)
                | Ok(rest) => Ok([value, ...rest])
              }
            }
          };
          loop(items)
        | _ => Belt.Result.Error("expected an array")
      }
    ];
  | Builtin("string") =>
    [%expr string => switch (Js.Json.classify(string)) {
      | JSONString(string) => Belt.Result.Ok(string)
      | _ => Error("epected a string")
    }]
    /* Exp.fun_(Nolabel, None, Pat.var(mknoloc("json")), Exp.match(
      makeClassify(makeIdent(Lident("json"))),
      [
        Exp.case(
          Pat.construct(mknoloc(Ldot(jsJson, "JSONString")), Some(Pat.var(mknoloc("string")))),
          ok(makeIdent(Lident("string")))
        ),
        Exp.case(Pat.any(), expError("Expected a string"))
      ]
    )) */
  /* makeIdent(Ldot(jsJson, "string")) */
  | Builtin("bool") => makeIdent(Ldot(jsJson, "boolean"))
  | Builtin("int") =>
    [%expr number => switch (Js.Json.classify(number)) {
      | JSONNumber(number) => Belt.Result.Ok(int_of_float(number))
      | _ => Error("Expected a float")
    }]
  | Builtin("float") => [%expr number => switch (Js.Json.classify(number)) {
    | JSONNumber(number) => Belt.Result.Ok(number)
    | _ => Error("Expected a float")
  }]
  | Builtin("option") => 
  Exp.fun_(
    Nolabel,
    None,
    Pat.var(Location.mknoloc("transformer")),
  Exp.function_(
    [
      Exp.case(Pat.construct(
        Location.mknoloc(Lident("None")),
        None
      ), Exp.construct(Location.mknoloc(Ldot(jsJson, "null")), None)),
      Exp.case(Pat.construct(
        Location.mknoloc(Lident("Some")),
        Some(Pat.var(Location.mknoloc("value")))
      ), 
        Exp.apply(makeIdent(Lident("transformer")), [
          (Nolabel, makeIdent(Lident("value")))
        ])
      )
    ]
  )

  )
  | Builtin(name) => failer("Builtin: " ++ name)
};

let rec makeList = items => switch items {
  | [] => Exp.construct(Location.mknoloc(Lident("[]")), None)
  | [one, ...rest] => Exp.construct(Location.mknoloc(Lident("::")), Some(Exp.tuple([
    one, makeList(rest)
  ])))
}

let rec forArgs = (sourceTransformer, args, body) => {
  let (res, _) = args->Belt.List.reduce((body, 0), ((body, index), arg) => {
    let argname = "arg" ++ string_of_int(index);
    (Exp.match(
      Exp.apply(forExpr(sourceTransformer, arg), [(Nolabel, makeIdent(Lident(argname)))]),
      [
        Exp.case(
          Pat.construct(Location.mknoloc(Ldot(Ldot(Lident("Belt"), "Result"), "Ok")), Some(Pat.var(Location.mknoloc(argname)))),
          body
        ),
        Exp.case(
          patPassError,
          /* TODO annotate error */
          expPassError
        )
      ]
    ), index + 1)
  });
  res
} and forExpr = (sourceTransformer, t) => switch t {
  | Variable(string) => makeIdent(Lident(string ++ "Transformer"))
  | AnonVariable => failer("Non variable")
  | Reference(source, args) =>
    switch (source, args) {
      | (DigTypes.Builtin("list"), [arg]) =>
        [%expr
          (list) => switch (Js.Json.classify(list)) {
            | JSONArray(items) =>
              let transformer = [%e forExpr(sourceTransformer, arg)];
              let rec loop = items => switch items {
                | [] => Belt.Result.Ok([])
                | [one, ...rest] => switch (transformer(one)) {
                  | Belt.Result.Error(error) => Belt.Result.Error(error)
                  | Ok(value) => switch (loop(rest)) {
                    | Belt.Result.Error(error) => Belt.Result.Error(error)
                    | Ok(rest) => Ok([value, ...rest])
                  }
                }
              };
              loop(Belt.List.fromArray(items))
            | _ => Belt.Result.Error("expected an array")
          }
        ]
        /* Exp.fun_(
          Nolabel,
          None,
          Pat.var(Location.mknoloc("list")),
          jsonArray(
              Exp.apply(
                makeIdent(Ldot(Ldot(Lident("Belt"), "List"), "map")),
                [
                  (Nolabel, makeIdent(Lident("list"))),
                  (Nolabel, forExpr(sourceTransformer, arg)),
                ]
              )
          )
        ) */

      | _ =>
        switch args {
          | [] => sourceTransformer(source)
          | args => Exp.apply(
            sourceTransformer(source),
            args->Belt.List.map(arg => (Nolabel, forExpr(sourceTransformer, arg)))
          )
        }
    }
  | Tuple(items) =>
    let patArgs = makeTypArgs(items)->Belt.List.map(name => Pat.var(mknoloc(name)));
    /* let rec loop = (i, items) => switch items {
      | [] => ([], [])
      | [arg, ...rest] =>
        let name = "arg" ++ string_of_int(i);
        let (pats, exps) = loop(i + 1, rest);
        ([
          Pat.var(Location.mknoloc(name)),
          ...pats
        ], [
          Exp.apply(forExpr(sourceTransformer, arg), [
            (Nolabel, Exp.ident(Location.mknoloc(Lident(name))))
          ]),
          ...exps
        ])
    };
    let (pats, exps) = loop(0, items);
    Exp.fun_(Nolabel, None, Pat.tuple(pats),
      makeJson("array", Exp.array(exps))
    ) */
    let body = ok(Exp.tuple(makeTypArgs(items)->Belt.List.map(name => makeIdent(Lident(name)))));
    let body = forArgs(sourceTransformer, items, body);
    [%expr json => switch (Js.Json.classify(json)) {
      | JSONArray([%p Pat.array(patArgs)]) => [%e body]
      | _ => Belt.Result.Error("Expected array")
    }]
  | _ => failer("not impl expr")
};

let forBody = (sourceTransformer, coreType, body, fullName) => switch body {
  | Open => failer("Cannot transform an open type")
  | Abstract => makeIdent(Ldot(Lident("TransformHelpers"), fullName))
  | Expr(e) =>
    Exp.fun_(
      Nolabel,
      None,
      Pat.var(Location.mknoloc("value")),
      Exp.apply(forExpr(sourceTransformer, e), [
        (Nolabel, makeIdent(Lident("value")))])
    )
  | Record(items) => 
    Exp.fun_(
      Nolabel,
      None,
      /* Pat.constraint_( */
        Pat.var(Location.mknoloc("record")),
        /* coreType */
      /* ), */
      jsonObject(items->Belt.List.map(((label, expr)) => {
        Exp.tuple([
          Exp.constant(Pconst_string(label, None)),
          Exp.apply(
            forExpr(sourceTransformer, expr),
            [(Nolabel, Exp.field(makeIdent(Lident("record")),
            Location.mknoloc(Lident(label))
            ))]
          )
        ])
      }))
    )
  | Variant(constructors) =>
    Exp.fun_(
      Nolabel,
      None,
      Pat.var(Location.mknoloc("constructor")),
      Exp.match(
        makeClassify(makeIdent(Lident("constructor"))),
        constructors->Belt.List.map(((name, args, result)) => {
          Exp.case(
            Pat.construct(Location.mknoloc(Lident("JSONArray")), Some(
              Pat.array(
                [
                  Pat.var(Location.mknoloc("tag")),
                  ...args->Belt.List.mapWithIndex((index, arg) => {
                    Pat.var(Location.mknoloc("arg" ++ string_of_int(index)))
                  })
                ]
              ),
            )),
            ~guard=Exp.apply(makeIdent(Lident("=")), [
              (Nolabel, Exp.construct(mknoloc(Ldot(jsJson, "JSONString")), Some(Exp.constant(Pconst_string(name, None))))),
              (Nolabel, makeClassify(makeIdent(Lident("tag"))))
            ]),
            {
              let body = ok(Exp.constraint_(Exp.construct(mknoloc(Lident(name)), switch args {
                | [] => None
                | args => Some(Exp.tuple(args->Belt.List.mapWithIndex((index, _) => {
                  makeIdent(Lident("arg" ++ string_of_int(index)))
                })))
              }), coreType));
              forArgs(sourceTransformer, args, body);
              /* let (res, _) = args->Belt.List.reduce((body, 0), ((body, index), arg) => {
                let argname = "arg" ++ string_of_int(index);
                (Exp.match(
                  Exp.apply(forExpr(sourceTransformer, arg), [(Nolabel, makeIdent(Lident(argname)))]),
                  [
                    Exp.case(
                      Pat.construct(Location.mknoloc(Ldot(Ldot(Lident("Belt"), "Result"), "Ok")), Some(Pat.var(Location.mknoloc(argname)))),
                      body
                    ),
                    Exp.case(
                      patPassError,
                      /* TODO annotate error */
                      expPassError
                    )
                  ]
                ), index + 1)
              });
              res */
            }

            /* Pat.construct(
              Location.mknoloc(Lident(name)),
              switch args {
                | [] => None
                | [one] => Some(Pat.var(Location.mknoloc("arg0")))
                | many => Some(Pat.tuple(
                  many->Belt.List.mapWithIndex((index, _) => (
                    Pat.var(Location.mknoloc("arg" ++ string_of_int(index)))
                  ))
                ))
              }
            ), */
            /* makeJson("array", Exp.array([
                makeJson("string", Exp.constant(Pconst_string(name, None))),
              ] @ (
                args->Belt.List.mapWithIndex((index, arg) => {
                  Exp.apply(forExpr(sourceTransformer, arg),
                  [(Nolabel, makeIdent(Lident("arg" ++ string_of_int(index))))])
                })
              )
            )) */
          )
          
        })->Belt.List.concat([
          Exp.case(Pat.any(), expError("Expected an array"))
        ])
      )
    )
};

let declInner = (sourceTransformer, typeLident, {variables, body}, fullName) => {
  let rec loop = vbls => switch vbls {
    | [] => forBody(sourceTransformer,
    Typ.constr(
      Location.mknoloc(
        typeLident,
      ),
      makeTypArgs(variables)->Belt.List.map(name => Typ.var(name)),
    ),
    body, fullName)
    | [arg, ...rest] =>
      Exp.fun_(Nolabel, None, Pat.var(Location.mknoloc(
        switch arg {
          | Variable(string) => string ++ "Transformer"
          | AnonVariable => "ANON"
          | _ => "OTHER"
        }
      )), loop(rest))
  };

    loop(variables)
};

let jsonT = Typ.constr(Location.mknoloc(Ldot(jsJson, "t")), []);

let makeResult = t => Typ.constr(
  Location.mknoloc(Ldot(Ldot(Lident("Belt"), "Result"), "t")),
  [ t, Typ.constr(Location.mknoloc(Lident("string")), []) ]
  );

let decl = (sourceTransformer, ~moduleName, ~modulePath, ~name, decl) => {
  let lident = makeLident(~moduleName, ~modulePath, ~name);
  let typ = Typ.arrow(
        Nolabel,
        jsonT,
        makeResult(
        Typ.constr(
          Location.mknoloc(lident),
          decl.variables->makeTypArgs->Belt.List.map(Typ.var)
        ),
        )
      );
  let rec loop = (i, vbls) => switch vbls {
    | [] => typ
    | [_, ...rest] => Typ.arrow(
      Nolabel,
      Typ.arrow(
        Nolabel,
        jsonT,
        makeResult(Typ.var("arg" ++ string_of_int(i))),
      ),
      loop(i + 1, rest)
    )
  };
  let typ = loop(0, decl.variables);
  let typ = switch (decl.variables) {
    | [] => typ
    | args => Typ.poly(
      makeTypArgs(decl.variables)->Belt.List.map(Location.mknoloc),
      typ
    )
  };
  let fullName = transformerName(~moduleName, ~modulePath, ~name);

  Vb.mk(
    Pat.constraint_(
      Pat.var(Location.mknoloc(fullName)),
      typ,
    ),
    declInner(sourceTransformer, 
        lident
    , decl, fullName)
  )
};

