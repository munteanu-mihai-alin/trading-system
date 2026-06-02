// Param lists for the typed navigators. Anything that grows a route
// goes through here so screens get type-checked params.

export type RootTabParamList = {
  Live: undefined;
  Runs: undefined;
  Launch: undefined;
};

export type RunsStackParamList = {
  RunsList: undefined;
  RunDetail: { id: string };
};
