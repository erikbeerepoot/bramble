import { createBrowserRouter, Navigate } from 'react-router-dom';
import App from './App';
import NodeList from './components/NodeList';
import NodeDetailPage from './components/NodeDetailPage';
import VisualizePage from './components/VisualizePage';
import ValveGroups from './components/ValveGroups';
import Settings from './components/Settings';

export const router = createBrowserRouter([
  {
    path: '/',
    element: <App />,
    children: [
      {
        index: true,
        element: <Navigate to="/nodes" replace />,
      },
      {
        path: 'nodes',
        element: <NodeList />,
      },
      {
        path: 'nodes/:deviceId',
        element: <NodeDetailPage />,
      },
      {
        path: 'visualize',
        element: <VisualizePage />,
      },
      {
        path: 'valve-groups',
        element: <ValveGroups />,
      },
      {
        path: 'settings',
        element: <Settings />,
      },
    ],
  },
]);
